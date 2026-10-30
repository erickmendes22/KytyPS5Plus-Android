# Android: PC/fork shader performance backport (2026-10-30)

Base: `experimental/neptunia-game-category-resolution` at `9f1b8ee7`.
Target: `experiment/pc-fork-shader-performance`.

## Ported changes

1. [KytyPS5 216bd28](https://github.com/KytyPS5/KytyPS5/commit/216bd28bf65602806375bb5fddc2290c279a3a4f):
   replace repeated linear searches of scalar-resource planning's visited instruction list with
   an `unordered_set`. Preserve the original Phi cycle guard and visitation order. Android
   **already** had O(1) memory-index ownership counting: do not replace or duplicate it.
2. [KytyPS5 ae67e2d](https://github.com/KytyPS5/KytyPS5/commit/ae67e2db519f71a9cca0d5014ebeca4880649695):
   canonicalize AGC shader hash/decoded code to aligned code + literal buffer for valid
   `barefoot` footers; hash the complete binary for missing, unknown, or malformed footers.
   Retain full original source size for vertex-metadata parsing.
3. Regression tests for metadata-only binaries, literal-buffer distinctions, malformed-footers,
   and aliased resource memory indices.

## Preservation and limits

- Keeps all Android FEX memory-reader safeguards, Turnip v32 fixes, guest-readback
  coherence, and existing sparse memory-index cache.
- Does not change Vulkan queue submission, render barriers, guest memory collection,
  native tessellation workarounds, game patches or game detection.
- Not measured yet on Adreno. Expected benefit is reduced shader compilation,
  redundant shader cache entries and CPU shader planning work; it is **not** a proven
  increase in steady-state FPS.
- Build validation: GitHub Actions Android APK and native host shader regressions on the
  branch. A successful CI does not establish smartphone correctness.
- Compare same-scene first-launch stutter with a cold cache and repeat with warm caches.
  Keep titles, selected Turnip version and phone temperature matched.

## Follow-up PC/fork candidates (not integrated)

- Jetsku U59 descriptor-set reuse with lifetime-complete keys, bounded to submission tick:
  only after profiling and Android image aliasing tests.
- Jetsku prioritized shader warmup and compiled-program disk cache: use codegen/driver
  fingerprint, title isolation, atomic invalidation across Android builds.
- usa-ratt GT7 VK dynamic state filtering: requires exact pipeline/command-buffer
  invalidation; avoid copying desktop-only state ownership into Android.
- GT7 and chenxiao07 synchronization/GPU retirement changes: defer pending Android
  SIGKILL and memory reclamation tests.
