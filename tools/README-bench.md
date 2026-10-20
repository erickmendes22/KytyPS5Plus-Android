# Astro Bot gameplay benchmark

One command, one number set, the same for every agent:

```bash
APK=research/<your>_apk/<build>.apk LOCK=<your lock> SCALE=1.0 tools/astro_bench.sh <run name>
tools/bench_summary.py research/runs/<run A> research/runs/<run B>   # side by side + CSV
```

Output goes to `research/runs/<run name>/` (summary.txt, bench.log, JPEG screenshots, gzipped
logs). A run takes about 6.5 minutes on the tablet (title ~100 s, two cinematic skips, gameplay at
~260 s, 30 s settle, 60 s window). Exit codes: 0 ok, 1 no gameplay / app died, 2 disk < 1.2 GB,
3 thermal guard tripped (rerun it, don't fight the guard).

## What it does

1. `devlock.sh acquire $LOCK` (waits for other agents and for the thermal guard's cooldown).
2. Installs `$APK` inside the lock. **APK is required** (`APK=none` runs whatever is installed):
   the tablet is shared and other agents install their builds between your runs. The summary
   prints the `Source build` that actually ran.
3. Restores the save (`RESTORE`, default `research/saves/current`) with `tools/astro_save.sh`.
4. Launches with `--perf-stats --gpu-turbo --env KYTY_LIVE_KNOBS=1` (+ `--render-scale $SCALE`
   unless 1.0, + `$EXTRA_ARGS`).
5. Drives the game from screenshots (`tools/astrostate.py`: title / pause / gameplay):
   title → Cross (press any button) → Cross on memory card 1 → after ~55 s skips the intro
   cinematic (OPTIONS → wait for the RESUME/SKIP menu → D-pad down → Cross) and ~8 s later the
   crash-landing cinematic the same way → gameplay at the crash site (Astro standing in the sand in
   front of the "!" marker; lower-half sand fraction >= 0.8).
6. Waits `SETTLE` (30 s), then measures a `WINDOW` (60 s) with Astro idle: the window is marked by
   the number of `[perf]` seconds and `_perf_frames.txt` flips on the device at open and close, so
   the summary uses exactly those seconds/flips. Thermal state is read at open and close.
7. HOME (releases the --gpu-turbo request), force-stop, pulls `_perf_*` and `_kyty.txt`.

## The numbers (tools/bench_summary.py)

| Field | Source | Meaning |
|---|---|---|
| fps | `_perf_frames.txt` | flips in the window / their time span |
| frame ms p50 / p95 / max | `_perf_frames.txt` | flip-to-flip intervals |
| GPU ms/frame | `[perf-wait] per frame: gpu_busy=` | GPU time of a frame's command buffers |
| probe ms | `[perf-probe] gpu_ms=` (every 2 s) | fixed-work GPU clock proxy, ~4.5 ms when turbo holds; higher = clock dropped |
| cpu % | `[perf] cpu=` | process CPU, 100% = one core |
| present_call | `[perf]` | ms in vkQueuePresent |
| waits ms/frame | `[perf-wait] ... ms(count):` | gpu-thread / sem / guest wait items, per frame |
| busiest threads | `[perf] busiest:` | % of a core (top 4 per second, so partial) |
| thermal | `dumpsys thermalservice` | status, skin, GPU/CPU max at window open/close; guard trips |

The window's CPU profile (`_perf_profile.txt`, last 5 s of the window) can be symbolized with
`tools/profsym.py <run>/_perf_profile.txt.gz --lib libmain.so=<unstripped libmain.so of that build>
--lib libc.so=<device libc>` (keep the unstripped `.so` of every APK you benchmark).

## Reference numbers (2026-10-18, branch opus-gameplay2, Turnip v32, --gpu-turbo, Astro idle)

| Run | Build | Scale | fps | frame p50 / p95 ms | GPU ms/frame | probe ms | cpu % |
|---|---|---|---|---|---|---|---|
| gp2_b3_s10 | 5f86059 | 1.0 | 5.85 | 170 / 200 | 162.0 | 4.548 | 122 |
| gp2_b4_s05 | 5f86059 | 0.5 | 6.05 | 167 / 186 | 155.4 | 4.548 | 112 |
| gp2_b5_fix_s10 | e14b62b+CPU fixes | 1.0 | 5.84 | 169 / 207 | 161.7 | 4.548 | 115 |
| gp2_b2_s10 | e0be8f4 (p3-ndkfex) | 1.0 | 5.45 | 183 / 211 | 161.9 | 4.548 | 146 |

b3 and b5 (40 min apart, GPU-identical builds) differ by 0.01 fps; skin was 48-51 C in every window. A run started right after a 1.0 run tripped the thermal guard 55 s in (gp2_b6_base_s10_thermal, skin 58.3 C); the bench now aborts with rc 3 on a trip. Leave a few minutes between runs.

## Options

| Env | Default | |
|---|---|---|
| `SCALE` | 1.0 | `--render-scale` |
| `EXTRA_ARGS` | | more emulator args, comma separated |
| `SETTLE` / `WINDOW` | 30 / 60 | seconds |
| `MAX` | 900 | hard stop before gameplay (s since launch) |
| `RESTORE` | `research/saves/current` | save dir to restore; `none` keeps the device's |
| `NEWGAME` | 0 | expect no save (same drive; backs the save up at gameplay) |
| `PAD` | | `KYTY_LIVE_KNOBS` pad script sent at gameplay (see below) |
| `LOCK` | bench | devlock name |
| `TURBO` | 0 | 1 adds `--gpu-turbo` (A/B policy below) |
| `SWEEP` / `SWEEP_S` | / 15 | live-knob phases after the window (see the script header) |

Compare A/B runs back to back with the same `SCALE`, `SETTLE` and `WINDOW`; check the probe ms
(clock) and the thermal lines before believing a difference.

## GPU clock policy for A/B measurements (research/clock_policy.md)

The tablet's GPU clock jumps between two levels during a run: the `[perf-probe]` dispatch takes
**4.547 ms at full clock and ~6.01 ms when throttled** (x1.32, every region slows with it). It
happens with and without `--gpu-turbo`, usually once the skin passes ~55 C, and kgsl's clock
files are not readable without root, so the probe is the only clock signal. Two CS_PACK A/Bs
were inverted by it (b3_pack1, b3_pack2). For every GPU-side A/B:

1. Measure A and B **in one run** as interleaved live-knob phases (`SWEEP=`, ABAB or ABBA, 20-25 s
   each, at least 3 pairs), not as separate runs. The knob must be live (read per dispatch or per
   pipeline key) so both settings run in the same thermal state.
2. Use `TURBO=1` (the app's `--gpu-turbo`) and `COOL=36`; keep `WINDOW=30` so the sweep starts
   cool. 30 s window + 6 x 20 s phases fit before the skin reaches ~58 C (b3_turbo1).
3. Run `tools/clockstat.py <run> --region <hash>...`. Only phases marked `ok` (>= 85% of their
   probe samples at <= 4.60 ms) count; compare adjacent A/B pairs where both phases are `ok`, and
   report at least 2 such pairs. Drop the rest.
4. `@full` (GPU ms scaled by 4.547 / mean probe) is only a cross-check: it scales memory-bound
   work too, so it under-reads throttled phases by ~5%.
5. A bench window number from another run is only comparable when both windows are `ok`.
6. A knob that changes pipeline keys (e.g. `bvh_opt`, which re-keys every compute shader) recompiles
   on its first phase and stalls for seconds: run each setting once as a throwaway warm-up round,
   then measure the repeated round (b3_r3_pack1).

## Saves

Astro Bot keeps its save in save-data memory (`files/_SaveData/PPSA21567/sce_sdmemory/1000/
memory.dat`, 2 MiB of XML for memory cards 1-3). It is shared by every agent and gets reset
(e.g. 04:01 on 2026-10-18 another run left a fresh one). Backups live in `research/saves/`:

```bash
tools/astro_save.sh backup <name>     # device -> research/saves/<name>/
tools/astro_save.sh restore <dir>     # research/saves/<dir> -> device (force-stops the app)
tools/astro_save.sh show [<dir>]      # slot-1 progress flags
```

`research/saves/current` -> `20261018_0318_o2p4B`: memory card 1 started, opening sequence not
completed. The title still says NEW GAME 1 with it and the game replays the intro, so the benchmark
skips the two cinematics every run (that is reliable; the skip is what avoids the crash-landing
cinematic, where unskipped runs hit VK_ERROR_DEVICE_LOST). The game writes the save only when
the memory card is picked (New Game); 10 minutes at the crash site, including walking to the "!"
rocks and climbing with pad= scripts (gp2_s1_savehunt), produced no checkpoint write. Reaching the
real first checkpoint needs actual play through the crash-site tutorial.

## Input automation knobs (KYTY_LIVE_KNOBS=1, file `files/_live.txt`, re-read once a second)

- `pad=<ms>:<lx>,<ly>[,<btn>+<btn>];...` - timed left stick (0-255, 128 centre, y 0 = up) and
  buttons, e.g. `pad=8000:128,0;400:128,0,cross;3000:128,0` walks forward, jumps, keeps walking.
  `adb shell input` cannot hold the on-screen stick and press a button at the same time.
- `shake=<s>` - synthetic accelerometer + gyroscope shake (Astro Bot's motion-only "shake to fix
  Astro" shot after the crash; with on-screen controls nothing else can feed the motion sensors).

`tools/kylive.sh "pad=..."` writes the file.

## Crash-landing cinematic runs (device-loss scene)

`APK=<apk> LOCK=<name> tools/astro_cine.sh <run>` plays the intro (skipped at `SKIP_AT`=30 s after the menu) and the
crash-landing cinematic without --gpu-turbo, and writes `research/runs/<run>/RESULT` (PASS = crash site reached, LOST =
"GPU fatal", THERMAL = guard tripped, rerun). It cools the tablet inside the lock (`COOL_SKIN`, up to ~6 min), wakes the
screen, answers Play Protect with "Don't send", logs skin/GPU every 30 s (`thermal.txt`) and the mesh ring lines
(`ring_timeline.txt`). `SKIP2=1` skips the crash-landing cinematic as well (crash-site screenshots `s_hold_*` every 10 s of
`HOLD`, for tools/pinkscore.py). A run takes 4-11 min.
