# PC core backports for KytyPS5 Plus

Comparison: Android 0.1.0 core `41f0dd32` against PC `8291cfeb` (2026-10-24).
The common ancestor is `5dec5231`. The 177 PC-only history entries are not 177 missing fixes:
many have matching patches or equivalent Android implementations.

## This batch

| Upstream | Change | Android adaptation |
| --- | --- | --- |
| [f6f1ac8b](https://github.com/KytyPS5/KytyPS5/commit/f6f1ac8b) | Batch dependent constant folds to avoid repeated graph walks | Retain Turnip's configurable consumer-depth limit and ancillary lowering |
| [8502d8a7](https://github.com/KytyPS5/KytyPS5/commit/8502d8a7) | Request extended image usage only when storage usage needs it | Apply to the existing scaled/UBWC-aware backing allocator |
| [b46eeb78](https://github.com/KytyPS5/KytyPS5/commit/b46eeb78) | Keep AudioOut2 pacing outside the global state lock | Retain zero-based host handles; snapshot PCM/gains; wait for in-flight output before closing |
| [10a852c2](https://github.com/KytyPS5/KytyPS5/commit/10a852c2) | Pack half-precision lane pairs with a bit-field insert | Common SPIR-V path; no desktop-only feature required |
| [61234194](https://github.com/KytyPS5/KytyPS5/commit/61234194), [64186404](https://github.com/KytyPS5/KytyPS5/commit/64186404) | Eliminate full DPP mask checks and reuse lane ID | Preserve guest-wave lane numbering under Android wave slicing |
| [a3aece27](https://github.com/KytyPS5/KytyPS5/commit/a3aece27), [9dcff95e](https://github.com/KytyPS5/KytyPS5/commit/9dcff95e) | Correct DPP source bounds and avoid invalid old ancillary reads | Common translator and masked-write implementation |
| [919fac13](https://github.com/KytyPS5/KytyPS5/commit/919fac13) | Define read-first-lane behavior for an empty EXEC mask | Integer ballot extraction also preserves subgroup-isolated operation |
| [706aa030](https://github.com/KytyPS5/KytyPS5/commit/706aa030) | Exclude edge flags from NGG primitive vertex indices | Preserve existing Android native-mesh/compute fallback selection |
| [8291cfeb](https://github.com/KytyPS5/KytyPS5/commit/8291cfeb) | Parse vertex metadata once per mapped shader | Cache invalid metadata too; remapping refreshes the cache |

These are correctness improvements and reductions in CPU/shader work, not measured FPS gains.

## Verified already present or more complete

- BDA table initialization (`4644c529`), storage range limits (`cd4eab9d`), zero-stride payload handling (`fde0b98e`).
- Safe overlapping-image copies (`440245f6`): Android additionally respects compressed block extents and 3D depth. Preserve that implementation; add the upstream regression case.
- Shared same-block bitcasts (`75d7a431`) and byte insertion (`6ee2ef87`).
- Optional barycentrics (`890393fb`): Android already supports a geometry fallback; do not replace it with PC's warn-and-continue behavior.
- Numerous shader, HTTP, network, NGS2, and kernel fixes have identical patch IDs; no duplicate application.

## Deferred pending mobile-specific work

- The new x86 SRT walker and its fault handling cannot replace the FEX/ARM64 safe guest-memory reader directly.
- Bindless image heaps, GPU indirect argument reads, conditional metadata clears, and synchronization restructuring need combined capability/lifetime validation on Turnip.
- Cache slot/pipeline lookup redesign needs its dependency chain and Android cache/lifetime audit.
- Shader clock, host occlusion queries, MSAA storage and tiled readback need explicit device-feature checks and game validation.
- Remaining arithmetic/IR and HLE changes require dependency and regression review; absence from this batch does not mean they are unsuitable.
- Desktop Qt launcher, OpenXR and PC packaging changes are outside this Android core batch.
- Debug-name stripping (`a951a917`) is deferred until the fork's name-based shader regression checks are adapted.

## Validation

The workflow rebuilds the ARM64/FEX native core for these source changes; old native-core reuse is rejected by its source guard.
Native host tests cover shader CFG/metadata, AudioOut2 concurrency and all shader compilation cases in normal and fast-path modes. Host tests do not establish game or driver correctness on a phone.
Mainline Turnip V32, package identity, game settings and the no-rt preset are retained.

## Astro Bot device regression follow-up

Device reports from the `1f5c2de` core show both OdxAsyncLoader faults and a separate
`VK_ERROR_DEVICE_LOST` during rendering. The 10:11 report includes a copy of the old
10:01 native tombstone; that tombstone is not evidence of a second loader fault in the
GPU-loss session. Both no-rt patches applied successfully and Mainline V32 was active.

Two mobile-specific shader contracts were lost in the first batch and are corrected:

- Native first-lane selection must inspect all four host ballot words, even for a guest
  wave32 shader; subgroup-isolated and split-wave paths retain their own handling. An
  empty ballot receives bit zero before FindLSB, preserving the upstream EXEC=0 fix.
- Pixel DPP writes must retain DppUpdateU32 so the Android backend can guard absent host
  source lanes. Full guest masks and BOUND_CTRL do not establish host-lane presence.
  The upstream ancillary optimization remains restricted to other shader stages;
  unsupported retained raw ancillary operands are rejected rather than silently dropped.

These fixes address verified code-path incompatibilities. The device logs do not yet
prove they are the sole cause of the Astro Bot regression. A fresh native build and
shader regression tests are required, followed by device confirmation.
