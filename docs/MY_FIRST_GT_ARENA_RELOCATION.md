# My First Gran Turismo: fixed allocator arenas on ARM64

## Observed failure

The follow-up PPSA24156 diagnostic from 2026-10-24 16:54:22 uses core `e13f41e5`.
The first high fixed direct mapping now enables address folding, as intended.
The new SIGSEGV is a different failure: the game clears guest address
`0xf400000000`, correctly folded by FEX to host `0x7400000000`, but no backing
mapping exists there. The crash is during startup allocation, not memory pressure
after gameplay.

That host address is outside the emulator's owned guest address windows. The same
region can contain FEX JIT mappings. Extending the allowed window or using an
unchecked MAP_FIXED mapping there could overwrite emulator code.

The supplied executable's allocator backing helper at RVA `0x2f9290` calls the
kernel batch-map import (`0x3e41d80`) and ignores its failed return at `0x2f93a9`.
Its caller still receives the pointer and passes it to memset.

## Guarded compatibility patch

The bundled allocator has three adjacent **32 GiB virtual** arenas. Relocate them
coherently, preserving capacity, alignment, arena order and guest pointer arithmetic:

| Arena | Original guest base | New guest base | Folded ARM64 base |
| --- | --- | --- | --- |
| 0 | `0xe400000000` | `0xc400000000` | `0x4400000000` |
| 1 | `0xec00000000` | `0xcc00000000` | `0x4c00000000` |
| 2 | `0xf400000000` | `0xd400000000` | `0x5400000000` |

All new folded intervals fit inside the owned ARM64 user window, ending at
`0x5c00000000`. This is a virtual address relocation; it does not allocate 96 GiB
of RAM or change the physical commitment of individual allocations.

Changing only arena 2 is unsafe: malloc/free/usable-size dispatch computes
`(pointer - 0xe400000000) >> 35`. The patch changes all three initializer
immediates and all 24 matching classifier immediates together (27 sites total).
Classifier MOVABS prefixes are checked individually (`48 b8`, except `49 bf` at
`0xcca8e` and `49 bd` at `0xfc1b4a`). The independent counter page
`0xe3ffffc000` and unrelated bit masks are untouched.

The loader applies the patch immediately after the executable PT_LOAD is read,
before instruction/TLS rewrites, relocations, protection or guest execution.
It requires ARM64, main executable, TITLE_ID `PPSA24156`, segment RVA zero,
entry `0xfef5b0`, executable file size `0x3e4548c`, the complete pristine segment
fingerprint `FNV-1a64 4f0ea18498883c85`, and every expected opcode/immediate.
All sites are validated before any byte is modified. A mismatched build is left
unchanged and emits one bounded diagnostic. No FEX mask, kernel mapping ownership
rules or other game's executable is modified. The original game file stays intact.

Supplied SELF SHA-256:
`7e03d8f703f1cd7dc2cc9e6c55318450a6701725a6f66a183a6489e519bb7118`.

## Validation and limitations

- Standalone regression tests verify all-or-zero byte mutation, opcode, title,
  entry, segment and whole-executable fingerprint guards, truncated input,
  repeated application rejection, and preserved classification at arena boundaries.
- Offline validation of the supplied original SELF's executable segment passes
  the production guard and changes exactly the 27 expected immediate bytes.
- Host/Android CI must still compile the integrated loader. The game has not yet
  been run on the device with this relocation. Clearing this startup failure does
  not establish menu, race, graphics or audio compatibility.

The site list and full executable guard are intentionally build specific. A
different game update needs a fresh executable investigation, not relaxed guards.
