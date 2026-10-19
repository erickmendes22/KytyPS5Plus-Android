# GPU clock policy for A/B benchmarks (opusB3, 2026-10-20)

Tool: `tools/clockstat.py <run> [--region <hash>]...`. Rules: tools/README-bench.md, "GPU clock
policy for A/B measurements".

## What the clock does

- `[perf-probe]` runs the same dispatch every 2 s. It has two values in every run checked:
  **4.547 ms (full clock)** and **~6.01 ms (throttled, x1.32)**, with brief 5.2-5.8 ms mixtures at
  the switch. Every GPU region slows by about the same factor in the throttled state (b3_pack2:
  6856320a 16.1 -> 21.1 ms/frame, cae14211 1.6 -> 2.1, G-buffer passes +20-30%).
- kgsl clock files are not readable without root: `gpuclk`, `devfreq/cur_freq` and
  `gpu_busy_percentage` under `/sys/class/kgsl/kgsl-3d0/` all return "Permission denied"
  (logged every 8 s in research/runs/b3_turbo1/skin_timeline.txt). The probe is the only signal.
- It is not tied to one knob. Other agents' sweeps show the same throttled level with no change of
  setting: c12_r10_k6 has every phase at 6.0 ms, c12_r12_k7 has its first two phases at 6.0
  (both settings), and c12_r11_k6on alternates.
- `--gpu-turbo` does not prevent it. In b3_turbo1 the window was 8/17 full clock and the last
  phase 0/10, at skin ~57 C. Turbo still kept four 20 s phases in a row at full clock after a
  30 s window (the fifth was 9/11), which none of the non-turbo sweeps managed.

## Packed vs unpacked phases with their clock (CS_PACK, scale 1.0, v32)

`ok` = at least 85% of the phase's probe samples <= 4.60 ms. GPU = [perf-wait] gpu_busy per frame.

| run | phase | probe (full/n) | state | fps | GPU ms/f | 338b4505 | 3a8772db | 6856320a |
|---|---|---|---|---:|---:|---:|---:|---:|
| b3_pack1 (no turbo) | pack=0 | 4.550 (5/8) | THROTTLED | 11.29 | 79.4 | | | |
| | pack=1 | 6.012 (0/9) | THROTTLED | 9.50 | 95.3 | | | |
| | pack=0 | 6.011 (2/8) | THROTTLED | 9.71 | 92.1 | | | |
| | pack=1 | 4.549 (5/6) | THROTTLED | 6.83 | 88.5 | | | |
| b3_pack2 (no turbo) | pack=0 | 4.548 (9/10) | ok | 11.61 | 76.7 | 5.94 | 3.83 | 16.51 |
| | pack=1 | 6.012 (0/11) | THROTTLED | 10.67 | 84.4 | 1.28 | 0.82 | 21.08 |
| | pack=1 | 6.013 (0/11) | THROTTLED | 10.80 | 83.8 | 1.26 | 0.83 | 21.25 |
| | pack=0 | 4.547 (10/10) | ok | 12.17 | 75.5 | 5.96 | 3.87 | 16.11 |
| b3_turbo1 (turbo) | pack=0 | 4.548 (9/10) | ok | 12.08 | 75.0 | 5.99 | 3.86 | 16.13 |
| | pack=1 | 4.546 (11/11) | ok | 12.99 | 66.8 | 1.13 | 0.63 | 16.14 |
| | pack=0 | 4.546 (10/10) | ok | 12.26 | 74.6 | 5.95 | 3.83 | 16.00 |
| | pack=1 | 4.548 (11/11) | ok | 12.88 | 66.6 | 1.13 | 0.62 | 16.07 |
| | pack=1 | 4.548 (9/11) | THROTTLED | 12.76 | 68.9 | 1.15 | 0.65 | 16.73 |
| | pack=0 | 6.013 (0/10) | THROTTLED | 8.77 | 102.7 | 7.82 | 5.06 | 21.39 |

All four packed phases of b3_pack1 and b3_pack2 were throttled, and only two of their four
unpacked phases were at full clock. That made
CS_PACK look like a loss: pack2 went 11.6/12.2 -> 10.7/10.8 fps, and RT looked +5 ms. At full
clock in b3_turbo1, RT is unchanged (16.0-16.1 ms both ways) and packing wins in both full-clock
pairs.

## Options weighed

| option | verdict |
|---|---|
| Only phases with probe <= 4.60, >= 3 interleaved pairs | **Adopted** as the acceptance rule: >= 85% of a phase's samples at full clock, >= 2 ok adjacent pairs out of >= 3 measured |
| `--gpu-turbo` for A/Bs | **Adopted**: more full-clock phases per run, but not a guarantee, so the rule above still applies. Thermal: COOL=36, 30 s window + 6 x 20 s phases ended at skin ~57 C with no guard trip |
| Normalize GPU ms by the probe | **Cross-check only**: b3_pack2's throttled packed phases normalize to 63.4-63.9 ms vs 66.6-66.8 measured at full clock (-5%), and base to 74.3-75.5 vs 74.6-75.0. Memory-bound work does not scale with the core clock, and fps does not normalize at all |

## CS_PACK under the policy (b3_turbo1, 2 ok pairs)

| | fps | GPU ms/frame | 338b4505 | 3a8772db | 6856320a (RT) |
|---|---:|---:|---:|---:|---:|
| cs_pack=0 (mean of 2) | 12.17 | 74.8 | 5.97 | 3.85 | 16.07 |
| cs_pack=1 (mean of 2) | 12.94 | 66.7 | 1.13 | 0.63 | 16.11 |
| delta | **+0.77 (+6.3%)** | **-8.1** | -4.84 | -3.22 | +0.04 |
