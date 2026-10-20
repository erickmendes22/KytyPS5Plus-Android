# Astro Bot GPU-time ranking (opusB3, step 1)

## Scale 1.0 (authoritative): research/runs/c12_r9a_base6, a6e2216d, 12.0 fps, GPU 74.3 ms/frame

Window average of the per-5 s [perf-gpu0] top-60 lines (scratch rank.py), ms/frame = ms/s / fps.

| # | Region | Kind | ms/frame @1.0 | @0.5 (ue5f_astro_tip) | Notes |
|---|--------|------|-----:|-----:|-------|
| 1 | 6856320a | cs-indirect (RT / BVH walk) | 16.1 | 16.6 | does not scale with render scale |
| 2 | 3b809f9d | pass 1920x1080 c5d (G-buffer) | 9.0 | 5.1 | 2.6 passes/frame, ~49 draws each |
| 3 | 338b4505 | cs, 25-thread wave32 groups | 6.0 | 6.0 | 25/64 host lanes busy; no LDS/barrier |
| 4 | 3a8772db | cs, 25-thread wave32 groups | 3.9 | 3.7 | same shape as #3 |
| 5 | misc (barrier/WFI marks) | misc | 3.6 | 2.1 | 216 marks/frame |
| - | 5919afa6 depth prepass 1920x1080 | pass | 3.2 | 1.6 | |
| - | 2c354544 3840x2160 c1 | pass | 2.0 | | full-screen, 1 draw |
| - | 2936e5ec 2432x1368 c2 | pass | 2.0 | | 1 draw |
| - | f112e0ba | pass | 1.6 | 2.2 | |
| - | cae14211 | cs | 1.6 | 1.6 | |

By kind @1.0: pass 29.1, cs 21.6, cs-indirect 16.8, misc 3.6, mesh-cs (mesh emulation) 1.0 ms/frame.
Compute (cs + cs-indirect = 38.4 ms) is fixed cost: it is the same at scale 0.5 and 1.0; the
~14 ms the scale saves is all in passes. Mesh emulation is only ~1 ms/frame.

## Scale 0.5 (first pass, superseded)

Source: research/runs/ue5f_astro_tip (_perf_gpu0/1.txt, _perf_pipestats.txt.gz), integration a6e2216d,
13.8 frames/s, render scale 0.5, driver v32. ms/frame = (ms per second) / 13.8.

| # | Region | Kind | ms/frame | Notes |
|---|--------|------|---------:|-------|
| 1 | 6856320a | cs-indirect (RT / BVH walk) | 16.6 | 48 regs, 4 waves, 17817 instr (5726 nop), 172 scalar ldg.u32 |
| 2 | 338b4505 | cs | 6.0 | 5x5 = 25-thread groups, wave32, 131072x1x1 groups, no LDS/barrier; 25/64 host lanes busy |
| 3 | 3b809f9d | pass 960x540 (G-buffer) | 5.1 | 3.2 passes/frame |
| 4 | 3a8772db | cs | 3.7 | 5x5 groups, no LDS/barrier (same lane waste as #2) |
| 5 | f112e0ba | pass | 2.2 | |
| - | misc (WFI marks) | misc | 2.1 | |
| - | 5a10a907 | | 1.7 | |
| - | cae14211 | | 1.6 | |
| - | 70c651c0 | shadow pass | 1.6 | |
| - | 5919afa6 | depth prepass | 1.6 | |
| - | 32d3ce28 | | 1.4 | |

By kind: cs 20.8, pass 17.8, cs-indirect 17.3, misc 2.1, mesh-cs (mesh emulation) 1.1 ms/frame (sum ~59.4).

## Picks

1. Compute workgroup packing (KYTY_CS_PACK): run two guest <=32-thread wave32 workgroups in one
   64-lane host workgroup for #2/#4. Estimated saving 3-5 ms/frame.
2. RT pass knobs KYTY_BVH_VEC / KYTY_BVH_OPT as a cheap measured experiment for #1.

Driver-side codegen changes can't be measured (bench must use prebuilt driver v32), so not picked.
Mesh emulation is only 1.1 ms/frame, so not worth targeting.

## Measurements (scale 1.0, driver v32, no turbo, in-run live-knob phases)

GPU clock varies between two DCVS levels during a run ([perf-probe] 4.547 ms vs 6.013 ms for the
same probe dispatch), so per-phase GPU ms/frame is also given normalized to the 4.547 ms clock.

### RT ceiling (b3_pack2, phase `skip=6856320a`)

| phase | fps | GPU ms/frame | at 4.547 clock |
|---|---:|---:|---:|
| base (neighbouring phases) | 11.4-12.1 | 72.9-77.5 | 70.6-72.9 |
| RT pass skipped | 13.74 | 56.6 | 56.6 |

The RT pass is ~16-17 ms/frame, but skipping it gains only ~1.9 fps: the frame then takes
~73 ms with the GPU at 57 ms, so the CPU/GPU-thread side becomes the limit. Skipping it also
removes nearly all lighting (research/runs/b3_pack2/s_sweep_nort.jpg): it is not optional work.

### Compute group packing (b3_pack2, ABBA)

| phase | fps | GPU ms/frame | probe ms | at 4.547 clock | 338b4505 | 3a8772db |
|---|---:|---:|---:|---:|---:|---:|
| cs_pack=0 | 11.61 | 74.1 | 4.695 | 71.8 | 5.94 | 3.82 |
| cs_pack=1 | 10.68 | 81.5 | 6.013 | 61.6 | 1.28 | 0.82 |
| cs_pack=1 | 10.84 | 81.6 | 6.014 | 61.7 | 1.26 | 0.83 |
| cs_pack=0 | 12.05 | 72.9 | 4.548 | 72.9 | 6.01 | 3.90 |

The two packed dispatches drop from 9.8 to 2.1 ms/frame (4.7x, at the lower clock). Both packed phases
of this run were at the throttled clock (probe 6.01 ms), which made packing look like a loss.

### Compute group packing under the clock policy (b3_turbo1, --gpu-turbo, 2 full-clock pairs)

See research/clock_policy.md. Means of the two pairs where both phases were at full clock:

| | fps | GPU ms/frame | 338b4505 | 3a8772db | 6856320a |
|---|---:|---:|---:|---:|---:|
| cs_pack=0 | 12.17 | 74.8 | 5.97 | 3.85 | 16.07 |
| cs_pack=1 | 12.94 | 66.7 | 1.13 | 0.63 | 16.11 |

Packing saves 8.1 GPU ms/frame (-11%) and adds 0.77 fps (+6.3%) at scale 1.0. Screenshots of the
packed phases (b3_pack2 s_sweep_a1/b1, s_window_mid) match the unpacked ones. b3_pack3 lost the
device in the crash-landing cinematic on mesh-cs 2b3be82b, the known fault (not a packed shader). b3_pack4 was
stopped by the thermal guard during settle.
