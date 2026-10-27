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

## Validation

- CI: Android APK and native regression tests.
- Device: **must confirm** `last-launch.txt` has the single preset,
  guest output registration matches expected, video-out Vulkan backing
  shrinks, scene renders and advances, driver avoids `DEVICE_LOST`,
  and both `device_usage` and `host_available` improve under comparable
  gameplay. A successful build alone is not proof.
