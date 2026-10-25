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

## Implemented patch plan (opt-in)

The native loader now implements the binary plan in `src/loader/shPerformancePatches.h`.
It requires PPSA10112, a main executable with entry RVA 0x80, exact text length
0x57554cc and pristine-text FNV-1a64 0x10ca182d422310d0. It identifies the text
before instruction rewriting, then validates the complete original registration
instructions for every selected option after all segment/RELRO loads. Unsupported
builds/sites cause the entire selection to be rejected before patch writes.
No game file on storage is modified.

Eleven opt-in launcher presets are shipped under `assets/presets/PPSA10112`:
three engine screen percentages (75/50/25), and eight independent effect switches
(shadow quality zero, volumetric fog, SSR, SSAO, bloom, DOF, motion blur and enforced
RT zero). Effects combine with one resolution choice through separate KYTY_SH_*
launch variables, without the one-JSON-game-patch limitation. Conflicting resolution
selections use the existing first-preset-wins rule and conflict report. Resolution
presets explicitly choose host render scale 1.0 to avoid stacking game percentage
with the emulator's alternate-backing downscale. Output dimensions are unchanged.
Disabling presets restores original executable values on the next launch; their
scoped environment keys are cleared even if Android/SDL reuses the process.

### Override mechanism proved in the supplied binary

The console manager vtable at RVA 0x74b6eb8 resolves integer registration to
0x1217540 and float registration to 0x12175f0. They store the incoming flags at
object +0x18 and seed game/render-thread values at +0x44/+0x48. The flag priority
check at 0x1216120 masks both incoming and existing sources with 0xff000000,
compares them at 0x1216157 and accepts incoming >= existing. Its own source-name
table labels levels 0..9 Constructor, Scalability, GameSetting, ProjectSetting,
SystemSettingsIni, DeviceProfile, ConsoleVariablesIni, Commandline, Code, Console.
The patches set source 9 while preserving all low 24 flag bits. Thus ordinary
lower-priority engine/config writes cannot undo the selected values. An explicit
equal-priority console write can still override them.

This also covers pre-registration INI values: the CreatedFromIni path at
0x1216c60..0x1216c7a invokes the normal Set(value, old_source), which reaches the
same priority check. Merely patching a default without this would not be sufficient.

For resolution, only the `r.ScreenPercentage` registration's RIP-relative float
load changes, referencing an already-existing read-only constant containing 75,
50 or 25. Shared float constants themselves stay unchanged. `r.DynamicRes.OperationMode`
retains its existing zero default and is also assigned Console priority so a
lower-priority dynamic-resolution policy does not supersede the chosen percentage.
A percentage is not asserted to be any absolute 720p/540p value.

For fog, the exact external integer at RVA 0x7b27f4c changes 1->0 before guest
initializers run, and its reference CVar receives the same source priority. Direct
RIP-reference analysis found the reference registration and no direct immediate
store resetting that integer. Other effects change their individual EDX constructor
value to zero. RT already starts at zero, so its preset only enforces that value;
no RT performance improvement is claimed from the original diagnostic.

### Validation and limits

`ShPerformancePatchesTests` passes on GCC with warnings-as-errors and with
AddressSanitizer/UndefinedBehaviorSanitizer. It exercises all 1024 selection
combinations on synthetic segments and on the actual supplied executable segments.
It checks untouched options, original flag preservation, resolution targets,
priority ordering, exact changed-byte boundaries, identity guards, invalid options,
short inputs and all-or-nothing rejection of a late registration/fog-data mismatch.
The original eboot remains unchanged. Android tests cover shipped-manifest parsing,
combined launch arguments, no double host scaling, resolution conflicts and the
complete set of environment keys cleared between launches.

This is binary/logic validation. Visual behavior, stability, RAM reduction and FPS
benefit of each option still require a device run. Zero dynamic-shadow quality does
not remove baked lighting; SSAO/SSR switches do not establish that every alternative
AO/reflection path in every scene is disabled. No lower-resolution textures, shader
removals, guessed draw skips or arbitrary render-output changes are included.

### Isolated execution of original x86 registration/setter code

`docs/tools/probe_silent_hill_cvars.py` (Unicorn 2.1.4) independently executes
the patched registration blocks and original integer/float/reference constructors
from the exact SHA-256-checked SELF. In this bounded virtual machine the allocator,
help/name/string formatting, parser and manager insertion are explicit stubs; it
does not boot the game or emulate a GPU. The real constructor instructions seed
both copies with zero for all eight effect controls, 50.0 for screen percentage,
and zero for dynamic-resolution mode, retaining Console priority and original
low flag bits. The original integer and float setters and priority predicate
reject sources 0..8 without changing either copy. Source 9 reaches the real
value store (a parsed value of 3 supplied by the parser stub), demonstrating the
intentional equal-priority-console exception. For accepted writes the probe stops
after the game-thread store; render-thread propagation of that accepted override
is not being claimed. This complements byte-plan validation, not device rendering
or full manager/INI-startup integration testing.
