# KytyPS5 Plus Android — UFC 6 performance / Demon’s Souls lighting integration

## Source and game scope
- Android source: `erickmendes22/KytyPS5Plus-Android`, branch `test/wolverine-demons-android-20261105`.
- UFC 6 `PPSA23566`; Demon’s Souls `PPSA01341`.
- Source reference: `Jetsku/KytyPS5` U59 `descriptorSetReuse.h/.cpp`, GPL-2.0; fork-specific UFC 6 branches `fxandrei/KytyPS5-ufc6` and `pxtwxl/KytyPS5-UFC6`.
- Do not port already-present CPU-only unmap no-GPU-drain and dynamic depth bounds again.

## UFC 6 findings (test session 2026-11-20 18:01, older build 25095b14)
- CPU placement mode 17 already present; no measurable FPS improvement reported.
- `fan-auto` switched off at 5.5, on at 34.4 and off at 14.7 us per walk.
- GPU memory grew to 4361 MiB against 4608 MiB budget and many image evictions.
- UFC6's four audience render-branch patches were verified as applied.
- Temperature is **not** a performance recommendation for this project.

## Optimizations integrated
1. Port U59's bounded exact descriptor-set reuse for non-push layouts. Before each bind the renderer verifies the layout and ALL buffer/image/sampler descriptors; entries cannot survive their GPU recording tick. Enabled only by `KYTY_UFC6_DESCRIPTOR_REUSE=1`.
2. Fan-auto engage threshold 40 us/walk, disengage 18 us/walk (6 s dwell). This avoids enabling a costlier front/record handoff at the measured 34.4 us, while still parallelizing expensive scenes.
3. Retain established Android submit thread, timeline tracker, safe coalescing, depth bounds dynamic state, CPU-only unmap bypass, and crowd optimization; do not disable barriers or skip draws.

## Lightweight bottleneck metrics
`KYTY_UFC6_PERF=1` emits **one** `[UFC6_PERF]` log line per ~1 second from the GPU command lane.
- `pm4_cpu`: wall time parsing/recording guest graphics submission (contains nested waits; **not additive** to them)
- `blocked`: waiting for queued guest PM4 dependencies
- `submit_driver`: actual time in `vkQueueSubmit`, and calls/s
- `submit_drain`, `timeline_wait`, `finish`: synchronization/wait sites
- `descriptor_prepare` and `descriptor_update`: descriptor CPU work
- `gc`: GC time
- `sets_reused` / `sets_written`, `submits` / `work`: volume counters
- `fan_on` / `fan_off`: fan mode per PM4 batch
- `gpu_busy=not_measured`: **no fabricated GPU time**. Device GPU load remains visible in Android HUD. Optional explicit `--perf-stats` activates the existing Vulkan timestamp-backed `[perf-gpu]` tool when a deeper GPU measurement is needed; it is deliberately not enabled by default to avoid the signal sampler overhead.

## Demon's Souls
The prior commit `dd7ee67` integrates (1) exact GPU image alias producer ordering with refuse-ambiguous fallback, (2) broad compute/image-composite binding revalidation, (3) native BVH/SRT producer visibility, (4) buffer write retick at GPU recording time. This integration does not prove full scene lighting until the APK is run on Android.

## Validation and release discipline
Run native shader/Bluepoint tests, full emulator compile, Kotlin launch argument tests, Android native regressions, and verify APK signing fingerprint. Do not claim FPS changes from source tests alone. Do not emit intermediate APKs for every small change.
