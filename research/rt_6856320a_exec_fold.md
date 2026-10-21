# RT pass 6856320a: where its 16 ms go, and a dead-lane select fold (opusB3, analysis only)

Data:
- research/runs/c13_r13_memo/_perf_pipestats.txt.gz (a4197462-era build): ir3 stats and NIR.
- b3_pack1: the guest IR dump of 6856320a (KYTY_SHADER_IR_DUMP).

Tools: `tools/irstat.py`, plus an offline NIR fold + DCE simulation (scratch script, described
in section 3).

## 1. Shape (ir3 / NIR)

| | value |
|---|---|
| ir3 instructions | 17787, of which NOPs 5695 (32%) |
| registers / waves | 48 full regs, 4 waves per core |
| (ss) / (sy) sync | 577 / 136 instructions; est. stall cycles: SS 2375, SY 6924 |
| cat5 (tex) / cat6 (mem) | 136 / 269 |
| NIR bcsel | 1369; one predicate, `%1783`, alone drives **749** of them |
| NIR phi | 482 |
| loops | guest CFG 12 back edges, 6 structured loops (NIR: 3 loop constructs); 98 ifs |
| cross-lane | 5 read_invocation (lane from ballot_find_lsb), 9 shuffle, 15 ballot, 26 vote |

- **The dominant predicate is a bounds check.** `%1783 = (x >= ReadConst[0x73]) || (y >= ReadConst[0x74])`, with x and y taken from the workgroup and local ids. It is the guest's 2-D out-of-bounds test: `v_cmp`, then `s_and_saveexec` and `s_cbranch_execz`.
- **How we translate it:** guest EXEC becomes a per-lane predicate. Every VALU write inside the region becomes `select(in_bounds, new, old)`, which keeps the out-of-bounds lanes' previous register values. That is 1087 IR uses of `%1075 = !%1783`.
- **Those lanes never come back.** Every side effect after the check is masked by `%1075`, or by a predicate built as `%1075 AND ...`:
  - the 2 ImageWrites;
  - the LDS stack writes;
  - the buffer stores.

  So the preserved values in those lanes are never observed, except through a cross-lane read.
- **This is the same pattern V2 folded in the vertex shaders** (KYTY_VS_LIVE_EXEC, −48–61% VS instructions). The difference is that here the lanes really are inactive, so the predicate cannot be set to true. Only the register-merge selects can go.
- **The traversal loop itself must stay.** The guest's per-lane "still traversing" exec narrowing (`%8071`, `%32884`, …, each driving 40–60 selects) re-joins after the loop. It is real divergence, not dead lanes.

## 2. Proposed transformation: dead-lane select fold (IR pass, opt-in)

The pass looks for an exec predicate P that is established by an exec narrowing and is *monotone*: every later lane-predicated operation, up to the end of the program, uses P or `P AND x`. When P qualifies, it rewrites `Select(P, new, old)` to `new` everywhere.

Exceptions, where a select is not folded:
- **Cross-lane reads that can see an inactive lane.** The select's value must not flow, through ALU ops, into:
  - ReadLane with a non-ballot lane;
  - DPP, swizzle, permute or permlane;
  - a shuffle.

  ReadFirstLane and ballot-chosen lanes only read active lanes, so they are safe.
- **Writes are untouched.** Stores, image writes, LDS writes and atomics keep their P masks.

The pass must be checked (with a fallback when the check fails) on:
- that no exec restore re-includes P's lanes;
- the hash allow-list.

It would sit behind KYTY_DEAD_LANE_FOLD (default off). Through the live per-program key (as with `bvh_opt`), the same run can carry an A/B.

## 3. Offline estimate

The simulation replaces `bcsel %1783, A, B` with `B` in the NIR, then removes dead SSA values from the side-effect roots:

| | before | after | change |
|---|---:|---:|---:|
| live NIR values | 5171 | 4179 | −992 (−19%) |
| bcsel | 1273 | 563 | −710 |
| phi | 351 | 161 | −190 |
| ALU (fadd/fmul/…) | – | – | ~−30 (the old values are mostly earlier results, not extra work) |

Expected ir3 effect:
- Removing about 700 selects plus about 190 phi copies is roughly −6–8% of the non-NOP instructions.
- The larger lever is register pressure. Each removed select or phi kept an old and a new value alive across a region or loop. If registers fall from 48 to ≤ 40 (5 waves) on this latency-bound pass (SY stalls ≈ 6.9 k cycles against 12 k non-NOP instructions), it runs faster.

**Expected: RT 16.1 → about 13–14.5 ms/frame (−1.5 to −3 ms) at full clock.** If registers stay at 48, only about −0.5–1 ms. The frame is GPU-bound (about 71 ms GPU of a 78 ms frame), so this would show up directly as frame time, about +2–4% fps.

## 4. Other options ruled out or deferred

- **BVH emulation options (bvh_opt 0/8/31):** measured in b3_r3_pack1, no gain (16.1 / 16.6 / 16.7 ms).
- **Short stack, front-to-back child order, any-hit early-out:** the guest owns the traversal and stack (LDS writes under `%1075`); we only emulate one node test per call, so these would mean rewriting the guest's algorithm.
- **Half-rate RT with temporal reuse:** skipping the pass removes all lighting (b3_pack2 `s_sweep_nort.jpg`), so it would need reprojection. Large and risky; not now.

## 5. Plan (budget: 4 runs)

1. **IR pass and offline checks:**
   - an IR pass with unit tests (synthetic exec region; a cross-lane exclusion case; an exec-restore case that must not fold);
   - spirv-val through `--compile-only-fast-paths` on the device: one short lock session, not a bench.
2. **Run 1:** live A/B `dead_lane=0/1` with `KYTY_PIPELINE_STATS=6856320a`, giving ir3 instructions, NOPs, registers and waves for both variants, plus RT region ms per phase under the clock policy, plus screenshots to compare pixels.
3. **Run 2:** a repeat or a fix-up.
4. **Runs 3–4:** held back for a Vampire Survivors check and a core+sync validation run if the fold becomes a default candidate.
