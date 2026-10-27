# PPSA24156 Turnip v32 crash isolation integrated with memory fixes

## Integration source and objective

This branch merges the exact code in
`src/graphics/host_gpu/renderer/pipeline/shaders.cpp` from
`investigate/mfgt-turnip-v32-crash-isolated`, revision `240e0efb`,
which was confirmed by the user to advance My First Gran Turismo past
the previously crashing Turnip tessellation pipelines.

Integration commit: `3ef74b27`. The change consists of **244 added C++
lines and no deletions** relative to the memory branch's pre-integration
`9c1d59f7`. All memory collectors, VMA policy and graphical-memory
presets remain intact. The isolated Turnip branch is preserved.

## Stage boundaries / why this is NOT a final visual fix

The narrow match requires all of:
- Game title `PPSA24156`
- Vulkan vendor ID `20803`, device ID `1141178369`
- `driverVersion == 109060195` (captured whitebelyash Turnip v32)
- Native tessellation pipeline with active pixel stage

The intervention replaces the **fragment shader (PS)** with a minimal
fragment, the **tessellation evaluation shader (DS/TES)** with a shader
emitting fixed `gl_Position`, and the **tessellation control shader
(HS/TCS)** with a module that writes four tessellation levels and two
inner levels, all 1.0. The minimal HS/TCS uses OutputVertices=4
while the pipeline retains the original 24 input patch control points.
It requires both previous minimal stages to be active to maintain its
minimal interface.

The user reported that this branch **worked**. This is evidence the
Turnip crash can be avoided by removing the original tessellation
control program and its interfaces from the ICD compiler's pipeline,
not proof that original geometry, effects and image quality are
preserved. The code intentionally removes tessellated geometry outputs.
**Do not promote it to a normal release unchanged.**

Existing environment controls:
- `KYTY_GT_TURNIP_FRAGMENT_PROBE=0` keeps the original PS.
- `KYTY_GT_TURNIP_TES_PROBE=0` keeps the original DS/TES.
- `KYTY_GT_TURNIP_TCS_PROBE=0` keeps the original HS/TCS.

All default to on for the exact affected driver/title. Disabling
earlier probes automatically prevents the minimal TCS from being
applied because of stage-interface requirements. No other
device/game/driver receives these modifications.

## Now focus: turn crash avoidance into visual correctness

1. Keep the proven three-stage minimal combination as a known working
   reference and preserve exact driver/title scoping.
2. Confirm the merged memory branch reaches a comparable point in
   game startup, without crashes, and still collects memory telemetry.
3. Recover the HS/TCS output varyings required by original TES.
   A minimal shader must preserve interface locations, component
   types, array/patch decorations, output vertices and tess levels,
   or risk undefined data. Do NOT merely re-enable original TES/PS
   with a minimal HS lacking its input outputs.
4. Restore original TES and pixel graphics one stage at a time,
   checking the same previously crashing tessellation pipeline
   hashes and Turnip error site. Retain the minimal-stage fallback
   only where actually necessary.
5. A successful compilation alone is not a memory or graphics
   validation. Compare shader-stage logs, visible geometry, GPU RAM
   and crashes during a like-for-like game sequence.

## Observed earlier Turnip evidence

- Original shaders: `SIGSEGV` in `libvulkan_freedreno.so+0xc21168`.
- Minimal FS only: same fault (pipeline #69).
- Minimal FS + TES: same fault (pipeline #160, HS
  `d9596b2d7a2e4202`, 24 input patch points).
- Minimal FS + TES + TCS: reported by user to work in the isolated
  Turnip branch; merged unchanged into memory branch.
