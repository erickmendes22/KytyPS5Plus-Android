# Contributing to KytyPS5 Android

Start with [STATUS.md](STATUS.md) for what works, and [ROADMAP.md](ROADMAP.md) for what to pick up.

## Ground rules

- **Legal:** use only game dumps you own. Never commit game files, saves, eboots, keys or firmware.
- **Opt-in first:** new behaviour goes behind a `KYTY_*` env knob. It becomes a default (on Android / with Turnip)
  only after an A/B at the same GPU clock step shows a gain with no visual change. `=0` must always opt out.
- **Tests:** add a small unit test for pure logic (pattern: `tests/StreamDefaultsTests.cpp` and its
  `stream_defaults_tests` target in `CMakeLists.txt`). Kotlin app tests run with `./gradlew testDebugUnitTest`.
- **FEX:** `src/cpu/patches/fex-addressfold.patch` was AI-assisted, and FEX does not accept AI contributions.
  Never send it upstream to FEX.
- **Commits:** conventional messages (`feat:`, `fix:`, `perf:`, `docs:` …). Keep each change small and measured.

## Build

You need the Android NDK 28.2, CMake, Ninja, `glslangValidator` and Android Studio's JDK. FEX is fetched at a pinned
commit (`KYTY_FEX_GIT_TAG`), or point `KYTY_FEX_SOURCE_DIR` at a checkout.

```bash
cmake -S . -B _Build/android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release -DKYTY_GUEST_CPU_FEX=ON -DKYTY_BUILD_LAUNCHER=OFF
ninja -C _Build/android -k 0 kyty_emulator
cd android && ./gradlew testDebugUnitTest assembleDebug -PkytyNativeDir=<dir with libmain.so, SDL3 and adrenotools hook libs>
```

CI: `.github/workflows/android.yml` builds the release APK on every push to `android-boot-wip`, runs the
unit tests, and refreshes the `nightly` pre-release. A `v*` tag publishes a versioned pre-release. Release
signing uses the repository secrets `KYTY_KEYSTORE_B64`, `KYTY_KEYSTORE_PASSWORD`, `KYTY_KEY_ALIAS` and
`KYTY_KEY_PASSWORD`; without them, the release APK is unsigned.

If FEX patches change, re-apply them before building:

```bash
cmake -DFEX_SOURCE_DIR=$PWD/_Build/fex-src -DFEX_PATCH=$PWD/src/cpu/patches/fex-addressfold.patch -P src/cpu/patches/applyFexPatch.cmake
```

Then rebuild `_Build/android/fex/build`.

## Run and measure

- **Driver:** install a Turnip driver through the launcher's driver picker. Mainline v32 is the default.
- **Launch from adb:**

  ```bash
  adb shell am start -n com.kytyps5.android/.GameActivity --esa com.kytyps5.android.extra.GAME_ARGS '--game,<path>,--perf-stats,--printf-direction,File'
  ```

- **Output:** `--perf-stats` writes per-second lines to `files/_perf_log.txt`:
  - fps, frame time p50/p95, GPU ms/frame;
  - per-thread waits;
  - `[perf-probe]`, the GPU clock step. Compare only runs at the same step.
- **Live knobs:** `KYTY_LIVE_KNOBS=1` lets you flip knobs at run time through `files/_live.txt`, for in-run A/Bs.
- **Thermals:** start each run with the device cool (skin < ~38 °C). Stop above ~62 °C skin.

## Useful knobs

| Knob | Default (Android) | Purpose |
|---|---|---|
| `KYTY_GPU_FAN` | `auto` | SRT walk fan-out by measured walk cost; `0`/`1` force |
| `KYTY_RT_HALF` | `2` (Turnip) | Half-rate RT lighting, with edge re-trace (`1` = no re-trace, `0` = off) |
| `KYTY_STREAM_PROMOTE` | `2` | Promote unchanged small CPU-written reads off the stream path; `0` off |
| `KYTY_STREAM_CACHED` | `1` | Host-cached stream buffer for small reads |
| `KYTY_SRT_MEMO` | `auto` | SRT materialization memo while fan-out is off |
| `KYTY_CS_PACK` | on (Turnip) | Pack two small workgroups per host workgroup |
| `KYTY_TITLE_WORKAROUNDS` | off | Per-title workaround lines |
| `KYTY_VK_VALIDATION` | off | Vulkan validation layer (core+sync) |
| `KYTY_PERF_GPU_TOP` / `KYTY_PERF_STACKS` | off | Per-region GPU time / GPU-thread stack sampling |

## Reporting a game

Open an issue with:

- the title ID and version;
- the device, the driver and the build commit;
- how far it gets, with fps at a given point;
- `files/_kyty.txt` (zipped) and a screenshot.
