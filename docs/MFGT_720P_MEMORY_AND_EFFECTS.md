# My First Gran Turismo PPSA24156: 720p VRAM experiment and non-resolution effects audit

## Input and scope

- User-supplied PS5 SELF `eboot(20261027-135440).bin` / previously supplied
  `eboot(20261024-184545).bin`: byte-identical, 109,163,961 bytes.
- Full SHA-256: `7e03d8f703f1cd7dc2cc9e6c55318450a6701725a6f66a183a6489e519bb7118`.
- Exact title `PPSA24156`, version `01.010.000`.
- The GoldHEN-format executable patch refuses other title/version/process and
  validates original instruction bytes before writing.
- This branch includes the existing GPU-written image collector, emergency
  eviction and 32 MiB VMA-block policy; **none** is removed.

## October 27 device evidence

From user diagnostics:

| | 124834 | 125429 |
| --- | --- | --- |
| Preset | old `native720` binary patch | old `mem50` renderer scale |
| Core | `e9d42542` | `e9d42542` |
| VMA blocks | 32 MiB | 32 MiB |
| Initial video-out Vulkan allocation | 3 images, ~95 MiB | 3 images, ~24 MiB |
| Progress | ~pipeline #153 | ~pipeline #356 |
| Failure | `VK_ERROR_DEVICE_LOST` at queue submission | RAM pressure to 95 MiB available; no proven Android LMK |
| Important | 720p *instructions* were patched, but 4K video-out backing remained | old 50% host scale reduced video-out memory; was NOT an unpatched control |

Comparison is not a controlled A/B test: the scenes/times and subsequent pipeline
progress differ. There is **no** patch-free baseline in this pair.

The old 720p JSON changes eight confirmed instructions at RVAs
`0x2f1e79/83/8d/92` and `0x2f1ee8/f2/fc/1f01`, inside two
display-target setup branches. The game's external video-out registration is
separate from these writes. The first branch loads 3840 x 2160; the second
loads 1920 x 1080. The patch replaces their size constants with 1280 x 720,
but device logs prove this alone did NOT remove the 4K-backed images.

## Corrected experimental preset

Only `native720` is shipped. Unlike the previous version, it **combines**:
1. The original identity-checked eight-instruction guest executable patch.
2. A Vulkan image-backing render scale of 0.33333334, so an externally
   registered 3840 x 2160 video-out image requests roughly 1280 x 720
   *host* backing (one ninth of the pixels).
3. The existing GPU written-image eviction / emergency memory collector
   and 32 MiB VMA blocks.

This is a **hybrid** experiment. Do not claim that all guest render targets
or the game's own internal renderer become true native 720p. It may lower
quality, double-scale some paths and is not proved stable on the user's
Adreno/Turnip setup. The `VK_ERROR_DEVICE_LOST` observed at pipeline #153
was a queue-submit failure, not demonstrably caused by the original patch.
It must be retested on device.

The app's `last-launch.txt` now records **effective** arguments, after
presets have been merged, including `--game-patch` and `--render-scale`.
Native `GT VIDEOOUT` logs also record the guest requested image dimensions
and scale, with an estimated backing size. Compare actual `videoout`
allocation in GPU-memory telemetry; estimated dimensions are not proof.

Removed due to no demonstrated benefit: `mem50`, `mem75`,
`native1080`, and the `sceKernelGetOperationMode` submode override.

## Other native graphical-patch candidates investigated — NOT shipped

The same SELF includes exact names for:
- Shadows: `disableShadow`, `enableGlobalShadows`, `GlobalShadowVramSizeActual_`,
  `shadow_vram`, `SHADOW_CASCADE_NUM`.
- Ambient occlusion: `disable_ssao`, `enableAmbientOcclusion`,
  `SsaoSpatialFilter*`.
- Ray tracing and reflections: `raytracing`,
  `enableRayTracingSpecular`, `reflection*`.
- Fog and effects: `volumetricFog`, `disableRenderCloudBuffer`,
  `no_motionblur`, `SceneMotionBlur`, `disable_glare`.

These strings, by themselves, **do not identify safely mutable instructions**
or prove that the engine checks the setting before texture allocation. For
example, `disable_car_mirror_aa` is referenced in a configuration-registration
sequence around RVA `0xbab08a`; its neighbouring immediate `0x3c9`
cannot safely be treated as a Boolean flag without decoding the constructor
and downstream consumers. Many other strings are in engine metadata tables
with no direct RIP-relative code reference at the string address.

Do not add guessed no-shadows/no-SSR/no-RT patches: skipping draw calls or
zeroing unknown globals can preserve the expensive textures, corrupt shader
state, or cause memory/device crashes. A viable follow-up patch needs exact
producer/consumer traces, original-byte matching and evidence that the
game ceases allocating the corresponding GPU resource before shipping it
as an opt-in preset.

GT7's published performance mode reduces or omits some effects, including
RT in gameplay; that does not establish that My First Gran Turismo executes
the same paths, or that a guessed RT-disable would reduce its memory.

## October 27 follow-up: direct x86-64 instruction/reference audit

After the initial string scan, the exact 109,163,961-byte SELF was
mapped using its embedded ELF64 header at file offset `0x1a0`. The
executable has a 0x4000 load-offset bias for its RX segment. References
were scanned with the actual RIP-relative `REX.W + LEA reg, [rip + disp32]`
instruction shape, and disassembled with `objdump -b binary -m i386:x86-64`.

Concrete findings (all below are **SELF file offsets**, not validated
GoldHEN `offset` patch RVAs):

| Name / candidate | ELF/SELF file offset | RX LEA reference found | Inspection |
| --- | --- | --- | --- |
| `volumetricFog` | `0x488cf57` | `0x00ef40b0` | Metadata registration: loads a descriptor and string, then calls common registration helper `0x0055d8c0` |
| `SceneMotionBlur` | `0x482a728` | `0x01e937bd` | Static member/descriptor registration, not the postprocess allocation or enable flag |
| `disable_ssao` | `0x48f14d5` | none through direct RIP-LEA scan | Name alone not a patch site |
| `enableRayTracingSpecular` | `0x48f129d` | none through direct RIP-LEA scan | Typed setting names also exist in mangled template symbols |
| `enableAmbientOcclusion` | `0x48d6ef8` | none through direct RIP-LEA scan | Typed setting metadata, not a proven allocation switch |
| `disableRenderCloudBuffer` | `0x48d6cea` | none through direct RIP-LEA scan | Typed `CloudSetting` symbols present; actual code consumer untraced |
| `disableShadow` | `0x4748182` | none through direct RIP-LEA scan | Also appears in `CrowdSetting` type names, likely crowd-specific rather than a global shadow cutoff |
| `no_rt` and `no_motionblur` | `0x4779ecf`, `0x4779ed5` | none through direct RIP-LEA scan | Candidate command/config symbols, not established branch points |

**Negative direct-LEA scan is not proof of no use.** The game can access
strings via hashing, indirect tables, pointers, or other instruction
encodings. This audit does establish why string-only offsets must not be
patched. The two confirmed LEA examples point at **metadata
initializers** rather than resource allocation paths.

The PlayStation GT7 performance-priority setting already omits or
simplifies ray tracing for real-time racing; My First Gran Turismo's
specific rendering path remains to be proved. A speculative RT-off
binary patch is especially unlikely to solve all RAM growth and may
have no effect in that mode.

**No additional effect-disabling native patch has passed producer/
consumer tracing and original-byte validation yet.** Do not present
shadows / SSAO / RT / blur switches as confirmed functional patches.
They remain investigation candidates; keep `native720` as the only
exposed experiment.

## Implemented graphical-memory experiments (subsequent revision)

In addition to the single retained `native720` resolution preset, **two new
optional non-resolution patches** have now been implemented in the renderer,
scoped to the exact `PPSA24156` title and available independently. This
supersedes the earlier statement that only the 720p preset is exposed.

1. **`depthhalf` — GT large depth atlases 50%:** for square single-mip,
   single-sample depth images of at least 2048×2048, allocate host Vulkan
   backing at half the current render scale, **only when depth image blits
   are supported**. It targets memory used by shadow-like depth atlases and
   other depth-only rendering. A depth-only render pass uses the matching
   viewport/scissor/shader scale; mixed color/depth passes return to guest
   backing instead of mixing incompatible attachment sizes. The game does
   not stop drawing shadows, so do **not** label this “disable shadows”.
2. **`hdrhalf` — GT HDR effect buffers 50%:** frame-sized,
   single-sample, one-mip RGBA16F color buffers can use half the backing
   resolution in single-color passes. Multi-attachment/color+depth passes
   are never assigned incompatible framebuffer scales. It is a
   lighting/postprocessing allocation experiment, **not** a verified
   SSR/SSAO/RT disable.

The implementation checks actual format, size, block compression,
multisample/mip eligibility and Vulkan blit support. Eligible creations
produce bounded `GT EFFECT BACKING: kind=large-depth` or
`kind=hdr-color` diagnostic records including **guest and host**
dimensions and the `scaled` flag. The new modes are disabled unless
their individual presets are selected.

Selecting either mode together with `native720` multiplies backing
scale by 0.5 for eligible images (for example `1/3 * 1/2 = 1/6`
linear size). These are intentionally **aggressive experimental values**
and can cause visual defects, extra fallback blits, device-loss errors or
a lack of memory savings. They should be benchmarked separately before
combining. Do not merge into production or claim successful device
operation based on a passing CI build.

No binary shadow/SSAO/SSR/RT toggle has been shipped, since no
instruction-level control with proven prevention of source allocations
has been identified. This restriction remains necessary even though the
two Vulkan residency patches are now implemented.

## Validation

- CI: Android APK and native regression tests.
- Device: **must confirm** `last-launch.txt` has the single preset,
  guest output registration matches expected, video-out Vulkan backing
  shrinks, scene renders and advances, driver avoids `DEVICE_LOST`,
  and both `device_usage` and `host_available` improve under comparable
  gameplay. A successful build alone is not proof.
