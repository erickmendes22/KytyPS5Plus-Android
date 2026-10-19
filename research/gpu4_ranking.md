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
