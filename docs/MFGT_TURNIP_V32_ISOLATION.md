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

## 2026-10-27 13:37 user log: original HS still crashes with both other stages replaced

Input: `KytyPS5-Plus-diagnostics-20261027-133711.zip`, build `4fbaf52`.
The session log confirms `GT TURNIP PROBE` and `GT TURNIP TES PROBE`
for the native tessellation pipelines. Pipelines #158 (input control points
26) and #159 (16) returned `Success`, while pipeline #160 (24) entered
`vkCreateGraphicsPipelines` and SIGSEGV occurred in the SAME Turnip
function at `libvulkan_freedreno.so+0xc21168` (read from address `0x10`).
Guest HS hash: `d9596b2d7a2e4202`. The fragment AND tessellation
evaluation modules supplied to the ICD were minimal; only guest VS/HS
remained. This rules out a requirement for the original PS or DS/TES to
trigger the crash. It does **not** establish whether HS, VS or pipeline
state causes the driver bug. The ZIP also has a previous session reaching
pipeline #356 without the same termination; do not conflate both sessions.

### Third-stage isolation: minimal HS/TCS (commit 240e0efb)

A new default-on, device/title-scoped `KYTY_GT_TURNIP_TCS_PROBE` stage
replaces the guest HS with a minimal tessellation-control SPIR-V shader.
It writes only `gl_TessLevelOuter` and `gl_TessLevelInner` from
invocation zero (all levels 1.0), and deliberately emits none of the
original tessellation varyings, since the minimal TES reads none.

**Critical captured SPIR-V fact:** HS `d9596b2d7a2e4202` declares
`OpExecutionMode OutputVertices 4`. Meanwhile, the failing Vulkan
pipeline declares `patchControlPoints=24`. The shader emits **4 output
vertices**, distinct from the **24 input patch control points**. The
new minimal TCS preserves this distinction and does not silently change
the pipeline's input patch count.

All three stage probes are restricted to `PPSA24156`, vendor `20803`,
device `1141178369`, driverVersion `109060195`. The TCS module
requires the minimal TES and FS probes to be enabled; they are needed to
keep pipeline stage interfaces internally consistent. To A/B isolate:
`KYTY_GT_TURNIP_TCS_PROBE=0`, or individually turn off other probes.
The output will be severely incomplete (expected); **this is not a
release-ready fix**. The relevant log line is `GT TURNIP TCS PROBE:`.
The next Android ZIP should be checked against the same driver offset,
pipeline identifier, output and input tessellation counts.

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
