# KytyPS5 Android: status

_Last updated: 2026-10-23 · branch `android-boot-wip` (default)_

This is an Android (ARM64) port of KytyPS5, maintained as its own project in this repository. It runs PS5
games on an Android tablet. The x86-64 guest code runs on an embedded FEXCore JIT, and the guest GPU work is
translated to Vulkan on Mesa Turnip (Adreno).

Reference device: **OnePlus Pad 3** (Snapdragon 8 Elite, Adreno 830, 16 GB RAM), Mesa Turnip
"mainline v32" driver loaded through libadrenotools. No other device has been tested yet. Reports are welcome.

## Games

| Game | Title ID | State | Speed on the reference device |
|---|---|---|---|
| Vampire Survivors | PPSA21703 | **Playable** | ~60 fps |
| Totally Reliable Delivery Service (Unity) | PPSA28855 | **Nearly playable** | 28–35 fps in gameplay |
| Astro Bot | PPSA21567 | Runs, slow | 13–17 fps at native res; **16–20 fps** with Performance mode (opt-in) |
| Commandos: Origins (UE5) | PPSA14708 | Boots to the title menu, very slow | ~2 fps (turn on the `res50` preset, see below) |

Speeds are 3-second means at the same GPU clock step. The tablet throttles after a few minutes, which costs
roughly 15–25%.

![Astro Bot gameplay](screenshots/astro-bot-gameplay.jpg)

![Totally Reliable Delivery Service gameplay](screenshots/delivery-service-gameplay.jpg)

![Commandos: Origins title screen](screenshots/commandos-origins-title.jpg)

## Per-game presets (in the launcher)

The app ships known-good per-title settings. Each one is **off by default** and you turn it on per game in the
launcher (`android/app/src/main/assets/presets/<TITLE>/<id>/preset.json`):

- **PPSA14708 `res50`** (Commandos: Origins): UE5 ini overrides for 50% screen percentage. About 2.3× faster,
  and it removes magenta tiles.
- **PPSA21567 `perf720`** (Astro Bot, "Performance mode (720p, faster lighting)"):
  - patches the game's render-size tables to 1280×720 and the post-processing size to 1664×936
    (a `--game-patch` file);
  - runs the ray-traced lighting at half rate (`KYTY_RT_HALF=1`).
  
  Result: +14–20% at the same clock and ~35% less GPU time per frame. The image is softer and edges are blockier.

![Astro Bot native vs Performance mode](screenshots/astro-native-vs-performance.jpg)

Presets can carry env knobs (`KYTY_*` only), `--render-scale` and a `--game-patch` file (schema version 2).

## What was done (high level)

- **CPU:**
  - FEXCore embedded as the guest JIT, with an address fold for UE5's 512 GiB reservation (39-bit VA on Android).
  - Guest-native `rdfsbase`/`RDTSC` to stop UE5's HLE polling storm.
  - A zeroed root frame record so Unity's backtraces don't crash.
- **GPU correctness on Adreno/Turnip:**
  - Barycentric and lane fixes for the magenta particles.
  - Helper-lane election fix for an Astro GPU hang.
  - `ds_swizzle` and fold-depth fixes for UE5.
  - Defined mesh outputs and culling of bad vertices.
  - Splitting of oversized dispatches.
- **GPU speed:**
  - Shader IR simplifications (lane broadcast, select fold, quad DPP, FTZ, UBO constants, VS exec fold, dead-lane fold).
  - Packing small compute workgroups (`KYTY_CS_PACK`).
  - Half-rate RT lighting with edge re-trace (`rt_half=2`, default).
- **GPU front-end (CPU side):**
  - SRT walk fan-out to worker threads, switched automatically by measured walk cost (`KYTY_GPU_FAN=auto`).
  - A record/submit pipeline thread.
  - A bind memo and byte-precise readbacks.
- **Uploads (Delivery Service, 2026-10-23, default on Android):** these lift Delivery Service from ~27 to ~30–35 fps.
  - `KYTY_STREAM_PROMOTE=2` (with backoff) and `KYTY_STREAM_CACHED=1`: stream-path fixes for small CPU-modified reads.
  - `KYTY_SRT_MEMO=auto`.
- **App:**
  - Launcher with a game library (folders and `.exfat` images).
  - On-screen controls.
  - GPU driver picker.
  - Per-game presets.
  - A thermal-aware GPU clock policy.

- **Upstream sync (2026-10-23):** 69 of upstream KytyPS5's 155 newer commits are cherry-picked. They include:
  - no more global barriers for PM4 flush events (fewer render-pass breaks on Adreno);
  - zero-initialised BDA/fault tables;
  - FIFO guest-mutex handoff;
  - about 50 shader correctness and instruction-count fixes.
  
  The rest needs porting onto our multi-threaded GPU front-end (see ROADMAP).

## Where the time goes now

| Game | Bottleneck |
|---|---|
| Astro Bot (Performance mode) | **CPU-side GPU front-end.** The record thread and the SRT walk workers set the ~50–65 ms frame. GPU work is ~40–48 ms/frame. |
| Delivery Service | CPU: guest write faults on CPU-written buffers, plus per-draw record cost. |
| Commandos (UE5) | Memory traffic: uploads and copies of large guest buffers. It needs zero-copy uploads. |

The Adreno 830 is roughly ¼–⅓ of a PS5 GPU before translation overhead. A stable 60 fps on heavy PS5 games is
not realistic on this hardware; 25–30 fps is the practical ceiling for them.

See [ROADMAP.md](ROADMAP.md) for what is next and [CONTRIBUTING.md](CONTRIBUTING.md) for how to help.
