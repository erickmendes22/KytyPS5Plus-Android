# KytyPS5 PC/forks vs KytyPS5 Plus Android: performance audit (2026-10-30)

## Source repositories reviewed

- [KytyPS5 upstream](https://github.com/KytyPS5/KytyPS5)
- [Jetsku U59 performance fork](https://github.com/Jetsku/KytyPS5), especially
  [U59 mechanism/evidence notes](https://github.com/Jetsku/KytyPS5/blob/main/docs/CHANGES-U59.md)
- [Coder787 KytyPlus](https://github.com/Coder787-source/KytyPlus)
- [modhollow shader precompile](https://github.com/modhollow/KytyPS5)
- [TheCruZ GTA fork](https://github.com/TheCruZ/KytyPS5-GTA)

This is a *source comparison*, not an assertion of higher fps on Adreno. Desktop
measurements cannot be transferred directly to a Qualcomm Turnip renderer.
All upstream patches require GPL attribution and their own regression tests.

## Features already present in the Android branch (do not duplicate)

| Mechanism | Android implementation | Status |
| --- | --- | --- |
| Asynchronous shader compilation | `shaderWorkerPool.cpp`, `pipelineCache.cpp` | Implemented |
| Shader program warmup records | `shaderDiskCache.cpp` | Implemented; stores inputs, not compiled translation |
| Vulkan driver pipeline binary cache | `_PipelineCache` in shaderDiskCache documentation / renderer | Present; distinguish from guest translation |
| GPU command front-end work splitting | `gpuPipeline.cpp`, `walkFan.cpp` | Implemented |
| Repeated binding-state optimizations | `pipeline/bindMemo.cpp`, `textureDescMemo.cpp` | Some implemented |
| CPU-known empty indirect dispatch skip | `renderCompute.cpp::DispatchIndirect` | Implemented; checks GPU-written arguments first |
| Large dispatch splitting | `renderCompute.cpp::RecordGuestDispatch` | Implemented |

## Missing higher-value desktop/fork optimizations

| Priority | Mechanism and source | Android gap | Safe integration target | Risk/validation |
| --- | --- | --- | --- | --- |
| P1 | Vulkan **descriptor set reuse**: Jetsku `pipeline/descriptorSetReuse.cpp` | No equivalent file/whole descriptor-write reuse | Add per-submit/per-tick keyed reuse at `pipeline/descriptorHeap.cpp` and `descriptors.cpp`; compare complete descriptor writes, layout and resource lifetime | Medium; invalidate on tick/pool reset, texture image replacement and buffer remap; test repeated draws and graphics accuracy |
| P1 | **First-page texture lookup + precise dirty image query**: Jetsku `textureCache.cpp`, `cleanVerdictCache.h` | Android texture cache has different overlap/dirty bookkeeping and lacks fork's query helpers | Profile lookup counts, then port cache-validated fast paths without bypassing write coherence | Medium-high; texture aliasing/readback must remain correct |
| P2 | **Translated shader/program disk cache**: Jetsku `pipeline/programDiskCache.cpp`, `shaderCodeSnapshot.h`, `shader/recompiler/CodegenFingerprint.cpp` | Android warmup file stores original program inputs, redoes IR/SPIR-V translation | Persist compatible translated records with hash of codegen version, options and GPU assumptions | Medium-high; avoid stale shaders and renderer build mismatches; primarily loading/first use |
| P2 | **Queued/coalesced Vulkan submissions**: Jetsku `queueSubmission.*`; `KYTY_SUBMISSION_COALESCE` | Android frontend is split, but command scheduler does not have equivalent submit coalescer | Preserve completion gates and guest fence/label ordering in Vulkan scheduler | High; asynchronous compute/dependencies can regress on Turnip |
| P3 | **Separate command-recorder thread**: Jetsku `commandRecorder.cpp`, `commandStream.*`; `KYTY_CP_RECORDER` | Android has a GPU frontend but not that exact packet ownership and Vulkan recorder | Incremental port behind an opt-in switch, only after the submission model is validated | Very high; must not send references to ephemeral guest resources to other threads |
| P3 | **FSR 1.0 EASU + RCAS spatial upscaler**: KytyPlus | No equivalent full EASU/RCAS present output path | Place after a verified lower-resolution renderer output; preserve unchanged screenshot timing and aspect ratio | Medium; quality and image stability, not by itself an fps speedup |
| P3 | **Parallel load-time shader precompile**: modhollow | Android async compilation exists; prioritization/time-bounded warmup differs | Port prioritized load queue after translation cache | Medium; impacts loading/stutter, may increase thermal load |

## What not to port blindly

- Desktop X3D CCD/core-placement routines: cannot assume Android big/LITTLE cluster topology.
- AMD/NVIDIA RT assumptions, DirectX-only optimizations, arbitrary dropped draws,
  forced ray misses, no-op graphical effects.
- Copying entire U59 command processor in one merge: source has diverged significantly;
  sync/ownership contracts and Android FEX lifecycle would be invalidated.
- More experimental performance changes inside the first Neptunia resolution APK
  before verifying title/version guard and the selected image actually reaches the screen.

## Next incremental implementation order

1. Finish `PPSA02721` resolution presets and verify manifest/category in an installable APK.
2. Benchmark render CPU time and draw descriptor update count with the existing performance
   counters; implement **descriptor set reuse** with exact descriptor equivalence and
   lifetime guards as a separately revertible commit.
3. Add image-query fast paths after proof of coherent invalidation on the game's actual
   render path.
4. Improve shader translation persistence with codegen fingerprint, scoped to title and build.
5. Explore queued Vulkan submission and FSR 1.0 independently, with 720p/1080p A/B.

Performance outcomes require measured **same-scene**, same driver, same shader cache
state and GPU clocks on the Redmagic 10 Pro. No claimed fps uplift until that comparison.
