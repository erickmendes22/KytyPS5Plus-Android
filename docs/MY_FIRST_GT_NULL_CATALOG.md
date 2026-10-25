# My First Gran Turismo: null archive catalog after arena relocation

## Device evidence

Diagnostic `KytyPS5-Plus-diagnostics-20261024-200246.zip` identifies
PPSA24156 and native build `9df30114`. The session explicitly confirms the
three-arena / 24-classifier compatibility patch was applied to the verified
executable. Thus this crash is after the previous unmapped-arena failure.
The original supplied SELF SHA-256 remains
`7e03d8f703f1cd7dc2cc9e6c55318450a6701725a6f66a183a6489e519bb7118`.

At 20:02:35 -03:00, thread `FWRKR` (21742) receives SIGSEGV/SEGV_MAPERR at
address zero. Native PC is `0x78c5b88248` in FEX-generated code, not a native
renderer/driver frame. The decoded instruction chain is:

```text
0x78c5b88220: and    x20, x19, #0x7fffffffff
0x78c5b88224: ldapur x11, [x20, #0x58]
0x78c5b88244: and    x20, x11, #0x7fffffffff
0x78c5b88248: ldapur x4, [x20]        // x11 == x20 == 0
0x78c5b88250: and    x20, x4, #0x7fffffffff
0x78c5b88254: ldapur x20, [x20, #0x18]
```

`x19 = 0xc400b88670` is the guest owner. Its field at offset `0x58` is null.
Guest code in `0x903d4fca0` reaches the virtual lookup call:

```text
0x903d4fd98: cmp  qword ptr [r15 + 0x20], 0x10
0x903d4fd9d: mov  rdi, qword ptr [r14 + 0x58]
0x903d4fdae: mov  rax, qword ptr [rdi]
0x903d4fdb1: call qword ptr [rax + 0x18]
0x903d4fdb4: ...                     // intended return address
```

The lookup is reached when the request's cached entry index at `+0x28` is -1.
It resolves a filename and then compares an FNV-1a32 filename hash against
records at owner `+0x68`. The FEX shadow call stack contains guest returns
`0x903d4fc5b`, `0x9008b0632`, `0x903d496b4`, and `0x903e0e001`.

The last memory sample has driver usage 675 MiB and available system memory
5915 MiB. This is not the earlier signal-9 memory-pressure failure. These
samples do not prove all later game scenes fit RAM, but the observed failure
is a null catalog dereference during startup.

## Archive initialization and remaining hypotheses

Guest archive initialization is at `0x903d4eb30`. It can leave the filename
catalog uninitialized via several explicit failure paths:

1. `0x903d4ed90` opens, stats and reads the complete index using an injected
   filesystem interface. A negative open result, nonzero stat error, or
   incomplete read prevents publication of the index buffer.
2. The loaded buffer is transformed by `0x903d4ba70`; initialization checks
   decoded magic `0x122bb8f2` and decoded version `0x11160000` at offset `0x20`.
   A mismatch exits initialization.
3. `0x903d4efa0` validates the filename-index span using header offsets/sizes
   before constructing its object and assigning owner `+0x58`. Subsequent
   entry-size/count checks, offset fixups (`0x903d4f060`) and volume setup
   (`0x903d4f1b0`) can also reject the index.

Cleanup `0x903d4f7b0` clears the same field at `0x903d4f87d`. The tombstone
alone cannot distinguish incomplete initialization from cleanup/race, or
identify which initialization check failed. A null-branch bypass would hide
an unusable catalog; it is not a demonstrated compatibility fix.

The main application configuration contains `/app0/contents/gt.idx` at RVA
`0x5ba6d60`, referenced by startup configuration at RVA `0x1aa5b1e`.
The shorter `/contents/gt.idx` at RVA `0x47ed28b` is appended to a returned
add-on mount path in the DLC bootstrap around `0x903d521cf`, after
`AppContentAddcontMount`. It does not justify inventing a root filesystem
mount or redirecting arbitrary `/contents` paths to `/app0`.

## Index header transform

Raw `gt.idx` bytes are obfuscated. A raw header must be decoded before
comparing it with the game's expected magic/version. The first 72 bytes are
enough to decode the required header fields independently of the rest of the
file; the game transforms the first `0x800` bytes. It uses the standard
non-reflected CRC32 table for polynomial `0x04c11db7` (table RVA `0x4fe2010`).
Exact equivalent Python:

```python
import struct


def crc_table():
    table = []
    for value in range(256):
        crc = value << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
        table.append(crc)
    return table


def decode_index_prefix(raw):
    table = crc_table()
    seed = 0x1FD5B434
    decoded = bytearray()
    for offset in range(0, min(len(raw), 0x800) & ~3, 4):
        cipher, = struct.unpack_from('<I', raw, offset)
        crc = 0
        for shift in (24, 16, 8, 0):
            index = ((seed >> shift) ^ (crc >> 24)) & 0xFF
            crc = ((crc << 8) & 0xFFFFFFFF) ^ table[index]
        decoded += struct.pack('<I', (~(crc ^ cipher)) & 0xFFFFFFFF)
        seed = cipher
    return decoded
```

For decoded first word `0x122bb8f2`, the expected first raw little-endian
word is `0x4e89f72c` (bytes `2c f7 89 4e`). The seed's four-byte CRC is
`0xa35db021`. This equivalence was checked against the supplied executable's
CRC table and disassembled loop. It is not validation of the user's actual
index contents, which were not included in this diagnostic.

## Ruled-out assumptions and next evidence

- `Common::File::Read` and `Write` already call `AddressFold::FoldPointer`
  before archive/backend I/O. Raw guest pointers at the higher KernelRead/
  KernelPread call sites therefore do not establish a missing syscall fold.
- The filesystem stat wrapper at guest `0x900522900` invokes the KernelFstat
  import (`kBwCPsYX-m4`, stub RVA `0x3e44770`), then reads file size at offset
  72 of the output structure. That matches the current FileStat ABI.
  Other wrappers read offset 88 for block-size buffer allocation, not file size.
- The filesystem vtable at RVA `0x554eac0` uses open at RVA `0x67aec0`, queued
  read at `0xfe9d70`, and stat at `0x522900`. The index load performs actual
  guest worker/callback I/O; there is no evidence that an unresolved VR stub
  supplies this catalog pointer.
- AppContentAddcontMount checks real entitlement/directory state before
  reporting success. The global entitlement list has three legacy GTA V
  entries, but those names do not mount successfully without matching content.
  They are not established as this crash's cause.
- The recent HLE ring is from this new AGC session, but its retained window
  is already around sequence 9350-10370. It contains no open/stat/read/AIO or
  AppContent calls from the earlier index initialization. Its omission of
  those calls is not proof the index was never opened.

The next bounded diagnostic should record `gt.idx` open result and resolved
path, file size from stat, and read offset/requested/actual bytes. Capture the
first 72 raw bytes once for offline decoding. This distinguishes missing path,
short/failed read and invalid decoded header without enabling general
per-call logs. Until those results are available, no new game-executable
patch or broad filesystem behavior change is justified by this dump.

## October 25, 07:08 -03:00 diagnostic

`KytyPS5-Plus-diagnostics-20261025-070808.zip` identifies native core
`3ed4cde2`, not the earlier core described in the previous conversation's
summary. PID 5575 exits with signal 11 at 07:07:58. Crashed thread 5851 is
`FWRKR`; owner `x19=0xc400b88670`, null `x11=x20=0`. The native PC is
`0x740cc7b9c8`. The last sample has 6174 MiB available, 655 MiB RSS and
675 MiB driver usage. The arena patch was applied successfully. This is
the same null-catalog failure, not a new RAM exhaustion: the dump contains
`ldapur x11,[owner+0x58]`, followed by the faulting `ldapur x4,[x20]`
with x20 zero, and the intended guest return `0x903d4fdb4`.
No bounded `MFGT index I/O` messages were emitted.

The next diagnostic now observes the guest initialization boundary instead of
assuming that the file-open wrapper was reached. Only the executable accepted
by the full pristine-text fingerprint may receive this observer. Its relocated
vtable slot at RVA `0x554e398` must still point to RVA `0x3d4eb30`; otherwise
nothing is installed. It uses the existing guest callback bridge to execute
the original two-integer initializer exactly once, preserving its arguments and
return register. At most eight before/after records expose the archive path,
backend vtable and catalog fields. It does not bypass the null dereference,
retry initialization, replace file contents, or fabricate a catalog.

Before guest execution, a separate read-only preflight opens only
`/app0/contents/gt.idx` and reads 72 bytes through the same mounted host-file
backend. It logs existence, length, read success and decoded magic/version.
This is header compatibility evidence only: it cannot validate the full index,
archive volumes, asynchronous guest read completion or initialization ordering.
The header decoder was checked using a synthetic header encrypted with the
CRC table from the supplied executable. Tests reject all short prefixes,
corrupted magic/version and zero-filled input without mutating the prefix.

Interpret the next device result as follows:

- Missing/unreadable/short preflight: investigate installed content or path access.
- Incompatible decoded prefix: inspect the actual index/version before changing code.
- Compatible preflight with no initializer entry: investigate guest startup dispatch.
- Initializer enters but catalog remains zero: distinguish backend open/read and
  constructor/header checks using the backend identity and existing I/O trace.
- Catalog is nonzero after initialization but later null: investigate cleanup/lifetime.

The host CI retains compilation of the complete filesystem test executable,
but runs its existing `--mutex-only` mode without a Vulkan window. The previous
run failed SDL video initialization on the headless runner, not compilation.
The full Vulkan-dependent filesystem test remains available separately; passing
the headless subset is not evidence that the full filesystem suite ran.
