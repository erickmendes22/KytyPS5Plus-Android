# CapyPS5 global resolution integration — Android (2026-10-30)

Source: [Ghael-V/CapyPS5](https://github.com/Ghael-V/CapyPS5), notably
[2686b392](https://github.com/Ghael-V/CapyPS5/commit/2686b39274),
[b62095f3](https://github.com/Ghael-V/CapyPS5/commit/b62095f35f),
[c274c2d9](https://github.com/Ghael-V/CapyPS5/commit/c274c2d9b7), and
[b669ce08](https://github.com/Ghael-V/CapyPS5/commit/b669ce0883).

## Comparison with KytyPS5 Plus

**The Android core already had universal host-side `--render-scale`:** target-sized
Vulkan backing with guest-size ImageInfo, viewport/scissor conversion,
guest-coordinate recovery in shader IR/SPIR-V, resolved image copies, and
scaled/unscaled backing transitions. Unlike CapyPS5's first implementation,
Android's existing backend can restore incompatible or mixed render passes
to guest-size backing rather than rendering with conflicting attachment geometry.

Android's existing `MemoryTracker::TakeBdaSync` and
`BufferCache::SynchronizeBda` already upload the queued dirty regions.
Do NOT paste CapyPS5's initial full synchronisation rewrite onto them.

## This experimental branch

- Launcher options `Native`, `75%`, `50%`, `33%`, `25%` globally and per title,
  feeding the existing host `--render-scale` pipeline. There is **no** eboot
  version dependency. At a 3840×2160 guest render target, 33% is about 1280×720
  and 25% is about 960×540. Smaller or incompatible surfaces may stay native.
  The output window's resolution is independent from the internal render scale.
- Retire runtime application of `res*` and GT's `native720` resolution presets.
  The APK still carries original files for source provenance/rollback; they are
  not shown or applied, even when enabled in a previous version. On title
  reconciliation, the preset installer removes old managed configuration files,
  restoring its backups. Existing saved games are not touched.
- Astro's old `perf720` preset is retained as **half-rate RT lighting only**;
  its native eboot resolution patch is neither installed nor passed to core.
- Keep GT `depthhalf` and `hdrhalf` **effect-memory experiments** and all
  genuinely non-resolution performance/compatibility presets.
- Shadow atlas opt-in is now `KYTY_SHADOW_SCALE=match`, globally and per title.
  Only square 2D single-mip single-sample depth targets of at least 1024px
  with a supported transfer format qualify. They use the SAME host/guest
  scale as main color targets (50%, 33% or 25%), so per-draw shader push data
  remains consistent. At Native (100%) this option is inert. An independent
  shadow factor requires a shader ABI that can handle two different image
  scales simultaneously; the original separate 50% experimental mode was
  unsafe and has been retired. Old persisted 0.5 maps to match. Mixed render
  passes keep their guest-size fallback.
- Regression tests cover UI settings/arguments, retired game presets and
  independent RT/effect toggles. The ARM64 APK and native regressions rebuild
  in GitHub Actions.

## CapyPS5 features surveyed, not indiscriminately copied

- **Frame generation (e66f3419)**: inserts host-generated frames, not
  a native guest FPS uplift. It depends on Capy's present path and has no
  verified matching frame lifetime or HUD ownership on Android; deferred.
- **Early depth bypass, FPS limiter (b669ce08)**: useful as targeted debugging
  and thermal control but not a universal speedup. Separate from resolution.
- **WaitRegMem and synchronization changes (e7a40ed3, 02e3ec5e)**:
  the Android core already has different page-fault, FEX and Turnip handling.
  In particular, never replace proper waits with silent timeout bypasses.
- **Video-out display buffer handling (c274c2d9)**:
  Android's render-scale-backed image conversion and late `SetScaled`
  already handle images initially seen as video-out then as render targets.
  Device testing is still necessary for double-buffered games.

## Validation criteria

1. Install over prior release with the same signing identity; check settings
   persist and the app is not reset. Native 100% must match the prior renderer.
2. At 33% and 25%, use a 4K guest title and check backed resolution and
   GPU allocations in logs; 33% does not guarantee 720p for a title whose
   original internal resolution is not 4K.
3. Verify gameplay output, menus, video, depth rendering and screen transitions.
4. Check formerly enabled `res*`/native720 presets no longer pass `--game-patch`;
   confirm original user files restored by the managed-file installer.
5. For shadow scaling, compare depth-only and mixed attachment passes at
   native and 50%, including Vulkan validation and a Mali/Turnip driver.
6. Compare FPS and frame times with identical driver, scene, shader cache,
   thermal conditions. No FPS improvement is claimed without device measurements.

Do **not** merge or release until CI and on-device validation pass.
