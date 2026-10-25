# Silent Hill: The Short Message — executable patch investigation

## Input identity

User-supplied `eboot(20261024-233256).bin`, 148259949 bytes,
SHA-256 `2b83640bf4e1bd49ae6098c1cb586b95d80eaa989aeebe2fdfbd8fb0e6b767af`.
Its uncompressed SELF executable segment starts at raw offset 0x2c530,
RVA zero, length 0x57554cc. These are offsets for this executable only.
The `shpc` project strings and PPSA10112 diagnostic identify the investigation;
no guessed game-version string is used as a patch selector.

The table below records decoded registration instructions, not a shipped patch
or proof of the final runtime value after config/device-profile overrides.

| Setting | Verified registration | Observed initial value | Status |
| --- | --- | --- | --- |
| `r.RayTracing` | name LEA at RVA 0x262ff97, int registration via vtable +8 at 0x262ffad | EDX=0, flags R8D=0x24 | Default is already zero; a no-RT patch cannot be claimed to help without checking the final value. |
| `r.RayTracing.Shadows` | name LEA 0x28409cb, registration 0x28409e4 | EDX=1, flags 0x20 | This default alone does not establish active RT when the main RT switch is zero. |
| `r.ShadowQuality` | name LEA 0x122bcad, registration 0x122bcc6 | EDX=5, flags 0x60 | Need a complete consumer/override plan, not only replacement of its initial value. |
| `r.ScreenPercentage` | float load 0x122c034, name LEA 0x122c03f, registration 0x122c052 | XMM0=100.0 from constant RVA 0x6910658 | Engine percentage, not an absolute 720p/540p guarantee. |
| `r.DynamicRes.OperationMode` | name LEA 0x4025fac, registration 0x4025fc2 | EDX=0, flags 0x20 | Game settings can override this later. |
| `r.DynamicRes.MinScreenPercentage` | float load 0x39485c7, registration 0x39485e5 | 50.0 from RVA 0x6910280 | Bounds alone do not prove dynamic resolution is enabled. |
| `sg.ResolutionQuality` | float load 0x3e3b396, registration 0x3e3b3b4 | 100.0, flags ECX=0x80 | Need to trace its later write to the renderer CVar. |
| `r.VolumetricFog` | name LEA 0x2ca4321, registration via vtable +0x30 at 0x2ca433c | External integer storage referenced in RDX | Must trace both storage and runtime callers before claiming disabled fog. |

A shadow-quality consumer begins at RVA 0x2b794d0. It initializes a cached
CVar data pointer, reads its render-thread integer at +4 and clamps it to
[0,5]. Five direct call sites exist; at least one immediately overwrites EAX,
using the call for initialization before other inline work. Therefore replacing
only this helper's return is not established as a complete shadow-disable patch.
For example, caller 0x2809ac7 has a real zero-quality branch at 0x2809ad0.
Do not replace shared constant 100.0 globally: it has unrelated users.
Do not disable shadow-related draw calls by guessed shader IDs.

## October 25 07:14 -03:00 device sample

Diagnostic `KytyPS5-Plus-diagnostics-20261025-071430.zip` is PPSA10112,
core `3ed4cde2`, PID 7876. Arguments include `--render-scale 0.5` and
`--gpu-evict-written 1`; allocator block remains 64 MiB. The 07:14:25 exit
has reason 1 / status 0. Signal-11 tombstones in this ZIP belong to earlier
Gran Turismo sessions, not this Silent Hill session.

Across 21 GPU memory samples:

- Driver usage peaks at 5543 MiB; active VMA allocations peak at 5196 MiB.
- Lowest MemAvailable is 104 MiB. Peak sampled RSS is 4707 MiB and swap 551 MiB.
- Maximum VMA reservation minus active allocation is 1736 MiB.
- Incremental writeback counters total 300 windows / 1215 MiB.
- Final sample: driver 4628, VMA blocks 4553, allocations 3807 MiB;
  MemAvailable 188 MiB; no cold-written candidates.
- Late residency records classify 160 MiB as special (video out, depth or
  stencil-associated), with zero ordinary eligible bytes. The aggregate cannot
  distinguish those subcategories. It does not justify discarding them.

These are UMA-related counters; do not add process RSS to driver usage as if
independent physical allocations. A smaller scale does not necessarily reduce
all storage/sample textures, and the renderer retains alternate backing images
for scale transitions (`Image::SetScaled`). This log cannot isolate the scale's
net RAM effect because scene/timing differ from the previous native-scale run.

The next APK requests 32 MiB allocator blocks only for confirmed PPSA10112.
This targets retained reservation slack; it cannot remove the 5+ GiB peak live
resource demand. Its RAM effect still needs the next device test.
No unverified native-game resolution/RT/shadow offsets are enabled in that APK.
