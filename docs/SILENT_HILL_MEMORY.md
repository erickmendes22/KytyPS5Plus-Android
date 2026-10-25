# Silent Hill: The Short Message — Android memory investigation

Base: `c40d34c360cece78a672adf64aa1d95f774039ad` (includes the Android host-lane Astro Bot fix).
Input: `KytyPS5-Plus-diagnostics-20261024-134200.zip`, PPSA10112.

## Evidence and corrections

- The user confirms menu, intro and 3D gameplay, followed by RAM growing until the app closes.
- Latest launch: 13:31:25; latest process exit: 13:34:27, reason 2 / signal 9. Neither exit identifies the killer. This supports investigating memory exhaustion; it does not prove lmkd involvement.
- Session log ends at VS 203 / PS 281 / CS 115 / GS 14. No device-loss or allocation-failure message appears. These counts do not measure memory consumption.
- The older 12:04 tombstone belongs to an OdxAsyncLoader SIGSEGV. Do not use it as the current game's crash stack.
- **The budget implementation is already in this core's sources**, introduced by `1a06576f4587eab01f450371cdc7137bb94f5572`. Android defaults are auto budget, 64 MiB VMA blocks and GPU-written-image eviction disabled. It is incorrect to present this as a missing PC feature to port.
- `ConfigOptions::printf_direction` defaults to Silent. `LOGF` suppresses budget startup and periodic memory messages. Missing messages do not establish that the mechanism is absent or disabled. The Android stdout capture only captures messages actually emitted.
- A [PC report for the same title](https://github.com/KytyPS5/KytyPS5/issues/547#issuecomment-6097176024) also reaches gameplay then exhausts VRAM and system RAM. Its hardware and renderer differ; its measurements are not Android measurements.

## Existing integration and limits

`GraphicContext::CreateAllocator -> InitMemoryBudget` configures VMA and budget. At a suspend/frame boundary, `RenderContext::RunGarbageCollector` invokes texture GC, then buffer GC. Texture GC refreshes the shared budget first. Image retirement is deferred on the GPU timeline. The allocation-retry path protects its source image and performs eviction only after allocation failure.

The current collector is a target for reclaimable caches, **not a hard limit on all process memory**. Guest RAM, FEX/JIT allocations, shader/driver memory and live renderer buffers can exceed it. Per-frame collection may also be late during an allocation-heavy interval before another frame boundary. Do not simply invoke collection inside every allocation: its candidate selection would need to protect the caller's source image and respect renderer locks and lifetime ordering.

GPU-written images are deliberately retained by default. The upstream commit records a stall when an experiment evicted 55 written images; downloading an image also touches guest RAM. On a unified-memory device this can move memory pressure rather than remove it. Do not enable `--gpu-evict-written 1` globally without measuring total pressure and stalls.

The BDA table already has a sized-table path, subject to device safety features. The shader compiler releases the local SPIR-V result after creating a module. Do not assume AnyPS5's retained-SPIR-V problem or the old dense-BDA allocation is the culprit here without measuring those paths.

## Prepared implementation

1. Refresh the budget at most once per second even without `VK_EXT_memory_budget`. For unified-memory devices, use the smaller of driver headroom and `/proc/meminfo` MemAvailable. Unknown samples are distinct from zero; an explicitly disabled budget and discrete-GPU policy remain intact. Existing reserve, floors and eviction safety checks are preserved.
2. Emit budget startup and material budget changes despite Silent logging. Add native build identification to Android session startup.
3. Retain the existing ten-second memory summary cadence. Include process RSS, anonymous RSS, swap, system available RAM, VMA block/allocation bytes, texture categories, GPU-written image bytes, buffer bytes and evictions. A bitmask identifies missing host readings. **Do not add RSS and Vulkan usage together:** UMA accounting can overlap.
4. Keep written-image readback opt-in. No changes to shader semantics, guest-visible memory capacity or resolution are required for these first changes.

This is prepared on an isolated investigation branch. No APK is claimed fixed or device-validated.

## Validation and next decision

`GpuMemoryBudgetTests` covers the existing policy plus CPU pressure with optimistic driver data, missing extension, zero versus unknown readings, explicit disable, floors, driver pressure, proc parsing, invalid units and overflow. GCC C++20 with `-Wall -Wextra -Werror` passes. AddressSanitizer and UndefinedBehaviorSanitizer pass with leak detection disabled because this execution environment prevents LeakSanitizer from enumerating threads. CI includes this target alongside existing native regressions. Full Android/native compilation and gameplay validation are still required; neither was run for this investigation.

The next APK must include the rebuilt native code (the audited-native reuse script rejects changes under `src/`, so an Android UI-only repack is insufficient). Compare the same gameplay route and duration:

| Observation | Implementation decision |
| --- | --- |
| Image allocation growth with little eviction | Inspect retained GPU-written/active images. Test bounded writeback separately; do not discard authoritative data. |
| VMA block bytes remain high after live allocation drops | Investigate fragmentation/dedicated allocations and deferred frees; do not shrink blocks blindly. |
| Vulkan allocations stay stable while anonymous RSS grows | Investigate guest/FEX/compiler allocations. Renderer eviction alone cannot fix it. |
| Evictions occur, guest RAM rises and total pressure persists | Reject writeback-only cleanup as a solution. Investigate asset demand or guest-resident memory instead. |
| Pressure grows before frame-boundary collection | Add an allocation-pressure checkpoint with source-image protection and safe lock/timeline ordering, then validate transitions. |

Acceptance is sustained gameplay beyond the previous failure point with bounded memory, no GPU loss, and no new corruption or long readback stalls. A later crash alone is not proof of a fix.

## Device follow-up and bounded written-image eviction

The 15:41:15 diagnostic confirms native core 804bc1f4. The budget is active (4031 MiB),
but driver usage reaches 6659 MiB and MemAvailable falls to 102 MiB. The last sample has
5046 MiB of images, 3434 MiB GPU-written, and `evict_written=0`. The earlier session has
the same pattern with a 3584 MiB budget. This identifies a large retained image set, not
which individual images are cold. An ANR is also recorded; no GPU-loss message is present.

The follow-up enables `--gpu-evict-written 1` only when the Android launch has confirmed
PPSA10112 title metadata. Astro Bot and unknown titles retain their existing defaults.
It changes written-image GC to one cold image per pass, chosen for useful reclaimed size:

- 32 idle frames under pressure; 8 under critical pressure. Current/recent frames, bound
  images, render targets currently bound, presentation images, depth and stencil owners
  are excluded. GPU-only contents are copied before retirement.
- Copy limit 32 MiB normally, up to 64 MiB critically, always clamped to the capacity of
  the already allocated download buffer. No extra staging allocation and no unbounded
  oversized image allocation. Larger cold images are counted but retained.
- Staging reservation uses `allow_wait=false`; a busy reservation is skipped. Once a copy
  is recorded, its GPU tick and priority memory publication must finish **before** image
  untracking/retirement. This retains correctness; the completion wait can still stall.
  Limiting copy volume and image count does not guarantee a maximum wait duration.
- Ten-second diagnostics include cold written bytes and bytes skipped for size, so a lack
  of eligible old images is distinguishable from a working set that cannot fit.

Portable budget/size-limit tests pass, including sanitizers (LeakSanitizer disabled for
the environment restriction described above). Full native/Android CI and gameplay remain
required for these follow-up changes. This is an experimental cleanup, not a claim that
the device crash or ANR is fixed.


## 16:54 device follow-up: chunked cold-image writeback

The 16:54:15 diagnostic confirms `e13f41e5` and `--gpu-evict-written 1`.
The mechanism executes: an interval reclaimed 661 MiB across 156 images, including
32 downloaded images. However, `oversize_written=1226 MiB` remains and available
system RAM falls to 93 MiB. Latest recorded driver usage is 6168 MiB. The latest
exit report is reason 3/status 0, not the earlier signal-9 exit; neither the
absence of that exit nor interval eviction proves sustained stability.

Large eligible 2D images now advance by one window of at most 8 MiB per collector
pass, using the existing staging ring and a bounded linear scratch request.
Guest mip offsets, padded row pitch, array stride, packed tail coordinates and
render-target XOR phases are preserved. Previous guest padding is read before
the GPU overwrites active texels. The image stays registered/GPU-authoritative
until every window has completed and its priority writeback has published.
A busy ring reservation is deferred. Touching or changing the image resets the
cursor; a content version also detects CPU, buffer and GPU writes between passes.
Unregistering discards the cursor. Only completed images are retired.

The existing opt-in remains PPSA10112-only by default. Bound/recent targets,
depth/stencil, video-out, MSAA, unsupported layouts, 3D volumes, scaled backings,
and linear images requiring a color transform remain excluded. No new guest
capacity, shader semantics or global eviction defaults are introduced. Small
image transfers keep their existing size limits. The `oversize_written` field
now reports oversized candidates whose layout cannot use bounded windows;
`writeback_chunks` and `chunk_bytes` show actual incremental progress every ten
seconds. These values alone do not measure a frame-time improvement.

Validation: portable budget tests and image-window address tests pass locally.
The latter cover five 2D tile families, 1/2/4/8/16-byte and BC formats, mip tails,
three array layers, padding, XOR phases, linear layouts and 256 MiB images while
bounding both buffers. Real-image-to-tiled-window Vulkan byte-comparison tests
were added to `--gpu-tiler-only`; they require a capable device and were not
executed locally. CI compiles that test suite and runs the CPU address tests.
GPU completion waits remain: an 8 MiB copy limit does not impose a time bound on
a hung or heavily loaded GPU. Android gameplay confirmation remains required.

## Follow-up allocator granularity

For confirmed PPSA10112 metadata only, the launcher also requests
`--vma-block-size 32`, retaining GPU-written image writeback. Other titles,
including My First Gran Turismo and Astro Bot, keep their original policy.
The 202442 sample showed up to approximately 1.5 GiB of reserved allocator
slack after reclamation. Smaller blocks may reduce retained slack; they cannot
eliminate live resource demand or guarantee a particular RAM reduction. VMA's
existing dedicated-allocation preference and allocation-count safeguards remain
in force. Device validation of this change is pending. No resolution or effects
patch with unverified executable offsets is included.
