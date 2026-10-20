# GPU command thread: time split and a pipelining design (opusB3, round B4)

Data: `--perf-stats` wall-clock stack samples of the GPU thread (SDLThread, 1 ms samples, the last 5 s
of each run), symbolized with the unstripped twin of each build (research/c12/base6/libmain.so,
research/c13/k1/libmain.so). Each stack is assigned to the innermost frame that matches a stage
(tools/gputhread_split.py). Stacks are cut at 9 frames, so
the outer stage is sometimes missing; the inner frames always decide.

## 1. Split of GPU-thread wall time (scale 1.0, v32)

c12_r9a_base6 (a6e2216, memo off = the current default; 12.0 fps, 83.3 ms/frame,
`gpu-thread:process` 83.8 ms/frame, the thread is 100% busy). Per frame: 581 draws, 74 dispatches,
44 emulated-mesh draws, 92 passes, 17 submits, 1 synchronous download.

| stage | % of wall | ms/frame | main frames |
|---|---:|---:|---|
| SRT walk + materialize (C2's black box) | 61.7 | 51.4 | SrtWalker::EvaluateWide/EvaluateInst/EvaluateRawRead, MaterializeResources; reached from ProgramCache::FindPermutation (specialization picks the program) |
| binding prep | 16.7 | 13.9 | PrepareBindings, ResolveTexture, PreparedVertexBuffers, PrepareDrawRenderState, Texture/BufferCache lookups |
| waits (sync, fences, page protect) | 6.3 | 5.2 | CommandScheduler::Active/RunOperation, VirtualMemory::Protect |
| PM4 parse + dispatch | 5.8 | 4.8 | ProcessPm4, CommandProcessor::Draw*, Pm4Execution |
| readback (GPU->CPU, synchronous) | 3.3 | 2.8 | BufferCache::ReadMemory <- GuestGpu::SendCommandSync (guest-thread fault reads run on this thread) |
| program lookup (excl. walker) | 2.9 | 2.4 | GetGraphicsPrograms, GetGraphicsPipeline, cache hashing |
| Vulkan record | 2.9 | 2.4 | ExecutePreparedDraw's vkCmd*, barriers, driver frames |
| submit | ~0 | ~0 | vkQueueSubmit already runs on CommandScheduler's submit thread (`gpu-thread:submit` = that thread's time) |

c13_r1_trace (KYTY_SRT_MEMO=1 plus memo trace; 11.3 fps, 88 ms/frame, process 67.9 ms/frame):

| stage | % | ms/frame |
|---|---:|---:|
| SRT walk + materialize | 32.9 | 29.0 |
| program lookup (incl. memo lookups) | 19.2 | 16.9 |
| waits (incl. 17% idle: no guest submission queued) | 17.4 | 15.3 |
| binding prep | 14.3 | 12.6 |
| PM4 parse | 6.4 | 5.6 |
| Vulkan record | 5.3 | 4.7 |
| submit/flip | 2.4 | 2.1 |
| readback | 1.2 | 1.1 |

With the memo the thread is no longer saturated (process 67.9 ms, idle 20 ms/frame), but fps did not
rise: `guest:fault` went from 62.7 to 186.5 ms/frame (summed over guest threads). The next gate after
this thread is the guest side, not only the GPU.

## 2. Serial dependencies

What has to stay in order, and what can run ahead:

| dependency | strict? | consequence |
|---|---|---|
| PM4 parse: register state is a running state machine | strict | one front thread parses |
| SRT walk/materialize of draw N reads guest memory and the shader registers at draw N | per draw, but has no dependency on the bind/record of draws < N | **can run ahead** of bind/record, given a register snapshot |
| program selection (FindPermutation) needs the materialized specialization | follows the walk | goes with the walk |
| the walker reads memory that an earlier draw writes on the GPU (BufferCache GPU-dirty regions, sync download) | strict on that draw having been **recorded and submitted** | must detect it and drain the back stage (bubble) |
| binding prep: TextureCache/BufferCache state (render targets become images, uploads, write tracking) changes per draw | strict, in draw order | stays on one back thread |
| Vulkan recording into the current command buffer, render-pass begin/end, barriers | strict | back thread |
| ordered side effects: EVENT_WRITE_EOP/RELEASE_MEM labels, WRITE_DATA, DMA_DATA, ACQUIRE_MEM, flips, interrupts | strict relative to the GPU work | forwarded as ordered items to the back thread |
| WAIT_REG_MEM / CE-DE counters on memory the GPU writes; indirect args read on the CPU | strict | front waits for the back to drain (bubble) |
| synchronous commands from guest threads (fault readbacks, SendCommandSync) | after all earlier recorded work | run on the back thread between items |
| submit | already asynchronous (submit thread) | none |

## 3. Design: two-stage GPU thread

```
guest submissions --> [A: front thread]                         [B: back thread = today's renderer]
                      PM4 parse                                  binding prep (Texture/BufferCache)
                      register snapshot per draw/dispatch  ==>   pipeline lookup, Vulkan record
                      SRT walk + materialize + FindPermutation   ordered PM4 side effects, flips
                      (walker untouched, black box)              sync commands, readbacks
                                    bounded SPSC queue of PreparedWork items
```

- **Split point.** The renderer already has the seam: `RenderExecutor::Draw*` builds a
  `DrawRenderState` (programs, vertex/pixel input infos with their materialized resources) and then
  calls `ExecutePreparedDraw` (bindings, pipeline, record). A becomes everything up to the
  `DrawRenderState` (and its compute equivalent); B is `ExecutePreparedDraw`/the dispatch
  recording, unchanged.
- **Snapshot.** `ExecutePreparedDraw` still reads live registers (`buffer.GetUserConfig()`, render
  targets, CopyDepthToColor). The queue item carries the context/user-config/shader registers B
  reads, copied per item with copy-on-change blocks (580 draws/frame x a few hundred bytes each).
- **Queue.** Bounded SPSC queue: about one guest submission (or about 256 items) deep for latency.
  Non-draw PM4 packets with side effects are queue items too, so B replays everything in order.
- **Hazard check on A (outside the walker).** A keeps the set of ranges that queued, unrecorded items
  will write: storage buffers/images marked written in their materialized resources, and render
  and depth targets. At the walker's guest-memory read boundary (`ReadGuestForSrt` /
  `BufferCache::ReadMemory`), a read that overlaps the set, or that the cache marks GPU-dirty, drains
  B first, which then behaves exactly as today. This uses only the walker's read callback, not its
  internals, so it composes with C2's memo.
- **Drains** (A waits for B to empty): WAIT_REG_MEM on GPU-written memory, indirect args read on the
  CPU, the hazard above, guest sync commands, and end-of-submission fences the guest waits on.
- **Shared structures.** ProgramCache/PipelineCache get one lock, or a lookup on A with installation
  on B (they already cope with the async-shader worker threads). Texture/BufferCache stay
  B-only, except the walker's read path takes the cache's existing shared lock. PageManager is
  already used from guest threads and has its own locks.
- **Kill switch.** `KYTY_GPU_PIPELINE=0` keeps today's single thread: A runs B's work inline. Keep
  that default until the A/B checks are clean.

Rejected options:
- **Per-pass secondary command buffers on workers:** Vulkan record is only 2.4–4.7 ms/frame, so there
  is nothing to win.
- **Parallel walkers for draws N+1..N+k** (fan out stage A): the next step once the two-stage split
  exists and C2 confirms the walker is reentrant. It needs the same hazard set, applied per worker.

## 4. Expected gain

Without the memo (current default), 83.8 ms/frame of thread time:

| | A (parse + walk + program) | B (bind + record + readback + waits) |
|---|---:|---:|
| today, serial | 58.6 ms | 24.3 ms |
| pipelined + about 2 ms snapshot/queue overhead each | about 61 ms | about 27 ms |

- The thread gate goes from about 84 ms to about 61–64 ms, allowing about 1 sync drain per frame
  (`down ... (1 sync)`) plus the WAIT_REG_MEM/fence drains per submission. GPU time is 66.9 ms/frame
  (CS_PACK default), so the frame becomes GPU-bound at about 67–70 ms: **about 14.3–14.9 fps vs 13.0 today
  (+10–15%)**.
- With C2's memo, A drops to about 51 ms (c13 split, trace included) and B stays at about 20 ms, so
  the GPU (67 ms) and the guest side gate. Pipelining then mainly removes the 17 ms of idle/serial
  slack between A and B.
- Beyond about 67 ms/frame, only GPU savings (RT 16 ms) or guest-side work (`guest:fault`) help.

## 5. Risks

1. **Stale reads by the walker.** If A misses a GPU write (for example memory written by a
   compute shader through BDA/flat SRT pointers, which the materialized write set may not
   describe), A reads stale data where today it would have triggered a download. Mitigation: treat any
   BDA/`uses_dma` program as writing "everything it may reach" and drain; also an A/B pixel-compare
   run.
2. **The guest side becomes the gate.** c13 showed `guest:fault` tripling when this thread got
   cheaper. The gain may show up as GPU-thread idle rather than fps; measure fps and `guest:fault`
   together.
3. **Thread-safety of the shared caches** (ProgramCache/PipelineCache, BufferCache read path, LiveKnobs,
   PerfStats regions, debug labels, `LogDrawPhase`). Audit them; keep perf regions and labels on B.
4. **Drain frequency.** If the guest uses WAIT_REG_MEM or CPU-read indirect args often, A stalls for
   B and the gain collapses toward 0. Count the drains (`[perf-wait] pipeline_drain=`) before
   tuning.
5. **Register snapshot completeness.** Any register B reads that the snapshot misses becomes a
   draw-order bug. Mitigation: B reads only through the snapshot (compile-time: pass a const
   snapshot, not the CommandBuffer).
6. **Mesh emulation ring, stream buffers and the indirect fixup** reserve space at record time.
   They stay on B; A must not reserve.
7. **Fault/crash attribution:** GPU-hang reports and debug ops refer to submit/draw ids, which must
   stay assigned in B's order.
8. **Coordination with C2:** the walker must be callable from A (thread affinity, the memo's
   tables). Interface: the walker plus one read-callback hook; no walker internals change.

## 6. Results so far (opus-gpupipe, scale 1.0, v32, no turbo, clock policy)

Sizing probe (b4_probe1, `pipe:` section, window of 34 s): 670 items/frame, 92 k SRT walker reads/frame.
Simulated drains per frame with a 64-item window: 4.6 guest sync commands, 0 hazard reads of
queued writes, 0 reads of GPU-dirty memory, 0 CPU-read indirect args. Blocked WAIT_REG_MEM: 10/frame
(the ring suspends; not a drain). BDA *writers*: 0 in the title phase (b4_probe2, thermal void
before gameplay). b4_probe1 counted every `uses_dma` program (44/frame, mesh/BVH *readers*) as a
writer; fixed. Register snapshot: HW::Context 2672 + UserConfig 148 + Shader 1224 bytes per item
(~2.6 MB/frame); materialized resources ~1.6–2.2 MB/frame; the record path accessed the live
register blocks ~4.3 times per item, all through CommandBuffer's three accessors (covered by the
snapshot).

Two-stage pipeline, `gpu_pipe` live A/B (3 interleaved pairs, every phase at full clock):

| run | gpu_pipe=0 fps | gpu_pipe=1 fps | process ms/f (0 / 1) | pipe_drain | pipe_rec | pipe_walk |
|---|---|---|---|---|---|---|
| b4_pipe1 | 12.66 / 12.62 / 12.55 | 11.47 / 11.28 / 11.24 | 79.7 / 88.7 | 5.1 | – | – |
| b4_pipe3 | 10.77 / 11.04 / 10.61 | 10.11 / 9.94 / 9.51 | 92.9 / 102.2 | 5.8 | 1.7 | 1.7 |

Both A/Bs measured a mis-gated pipeline: PrefetchDraw sent every draw with CB_COLOR_CONTROL.MODE
!= 0 inline, which includes MODE = 1 (normal), i.e. nearly every draw. Only ~2 ms/frame of record
work actually ran on the record thread; the rest drained per draw, so the slowdown is the
handoff overhead. Fixed (only fast-clear elimination, resolve and FMASK/DCC decompression, plus
depth copies at MODE 0, go inline). Not re-measured: the frame is GPU-bound at ~71 ms with the
SRT memo (C2's c13_r13_memo: GPU-thread process ~55 ms), so the pipeline cannot move fps until GPU
time drops below that. Still opt-in (`KYTY_GPU_PIPELINE=1` / live `gpu_pipe=1`). Not yet run: the
core+sync validation run and the Vampire Survivors run with the pipeline on.
