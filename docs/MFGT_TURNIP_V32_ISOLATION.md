# PPSA24156 / Adreno 830 / Turnip v32: Vulkan pipeline-compiler crash

## Scope

This branch starts at the **1de4f216** recovery commit. It deliberately excludes
all memory collector experiments, HS hash quarantines, and later SPIR-V
size-pass / Vulkan-disable-optimization experiments.

The failure occurs *inside* `vkCreateGraphicsPipelines` in the external
Turnip ICD. An APK build passing CI does **not** demonstrate that it is fixed.

## Evidence from user diagnostics

- Device: Adreno 830, vendor **20803**, device **1141178369**.
- ICD: whitebelyash Mainline Turnip v32,
  Mesa 26.3.0-devel (git-9c7e022677), driverVersion **109060195**.
- Native signal: invalid read near address 0x10, PC offset
  **libvulkan_freedreno.so+0xc21168**.
- Repro inputs: `KytyPS5-Plus-diagnostics-20261027-072706.zip`,
  `20261027-092614.zip`, and `20261027-092741.zip`.
  Each contains `gt-pipeline-capture/` with full pipeline JSON and
  original SPIR-V modules. These user-owned diagnostics are **not** published
  in the repository.
- The captured crash pipeline contains native tessellation with
  16/17/24 patch input control points. One repeated set:
  LS=17ce9b3880b66cb1, HS=d9596b2d7a2e4202,
  DS=611c3eb1e528da72, PS=5adbebda98a38ae7.
- HS SPIR-V of that set has roughly **6,835 instructions**, around
  **159 OpPhi** and **three barriers**. The driver receives a complete
  pipeline (including a large fragment shader); the last-stage name
  alone is not proof that the hull shader compiler caused the crash.

Previous tests with serialization, driver-pipeline-cache bypass, Vulkan
disable-optimization and limited SPIR-V size optimization did not prevent
the crash. Blocking a couple of HS hashes only exposed more failing HS
variants. Do not repeat these as supposed fixes.

## Current diagnostic experiment: isolate the fragment stage

Only when ALL conditions match:

- Title ID: `PPSA24156`
- GPU vendor ID: `20803`
- GPU device ID: `1141178369`
- Vulkan driver version: `109060195`
- Native tessellation graphics pipeline with a fragment stage

...the pipeline compiler receives the unchanged VS/TCS/TES stages and a
minimal valid fragment module with an empty `main()` and no outputs.
The rest of the pipeline state, including tessellation input control
point count, remains unchanged. The original PS module is still compiled
on its normal path; only the Vulkan pipeline stage uses the probe.

Log prefix: `GT TURNIP PROBE`.
A/B override: `KYTY_GT_TURNIP_FRAGMENT_PROBE=0` disables this behavior.

**This is NOT a graphics fix.** Tessellated surfaces may render incorrectly
or disappear. Do not merge into release or default branch.

### Interpret results

- Crash remains at the same ICD offset after entering the probed
  pipeline: original fragment module not required; investigate
  tessellation/vertex stages or interactions elsewhere.
- Previously crashing pipeline compiles and execution advances:
  fragment compiler or stage-interface linkage implicated; next test
  should restore fragment stage and replace one tessellation stage
  individually.
- Vulkan rejects the probe shader/pipeline: the experiment is
  inconclusive. Do not call that a repaired crash.
- App runs out of memory or terminates for another reason: diagnose
  separately, without modifying this branch's memory settings.

Keep native pipeline capture ZIP exports from both the control and probe
runs to compare exact shader modules, patch-control-point counts and the
last entered pipeline. A successful CI build is only compilation validation.
