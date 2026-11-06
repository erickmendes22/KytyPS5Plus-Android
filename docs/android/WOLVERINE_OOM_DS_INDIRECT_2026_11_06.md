# OOM after oversized SSBO support; persistent Demon rendering failure

## Evidence

Wolverine diagnostics 20261106-154555 identify PID 17180 and Android exit `[OOM KILL] oom` at 15:45:25 UTC-03. Core 6603a8fb reached CS92, after binding a 2,868,903,936-byte arena in three 1-GiB Vulkan descriptor slices (shader 4aeaa757f8135f95). The former descriptor-range abort is absent. The last periodic sample before this transition reported 5,429 MiB host available.

`SynchronizeBuffer` previously packed every dirty byte of a view into one `UploadMemory` call. Uploads larger than the 128-MiB staging ring allocate a temporary host-visible buffer of that entire packed size. Thus the arena could demand both its device mirror and a second multi-GiB upload allocation, in addition to overlapping buffers awaiting GPU retirement. The log does not capture a precise allocation-time peak, so the OOM is confirmed but the exact share of each allocation is not.

Demon diagnostics 20261106-155144 confirm synchronous shaders and repeated rebind-after-join activity. They still show missing rendering according to the user. Therefore neither earlier change establishes resolution of the visual problem. The Android defaults still enable GPU indirect draws and targeted readbacks, whereas the desktop defaults use CPU-resolved indirect arguments and full readback synchronization.

## Changes

Buffer synchronization now uploads at most 32 MiB of dirty pages per batch, using the existing staging ring and its GPU-tick reuse waits. It preserves the entire guest view and the segmented shader descriptors. Boundaries are tracker-page aligned so clearing a dirty page in one window cannot discard the remaining bytes in the next. Image-to-buffer synchronization remains once over the original whole view after its CPU uploads. No guest buffer range is truncated.

PPSA01341 alone uses `--gpu-indirect 0 --targeted-readback 0`, preserving prior Bink/TSC, synchronous pipeline and buffer-join corrections. This restores the desktop argument/readback path as a correctness fallback; the new logs do not yet prove that this difference causes the missing scene. No unsupported claim of an Android rendering fix is made.

## Validation

MemoryTracker regressions exercise the production upload-window helper with actual dirty tracking, read-only and GPU-written ownership, partial first/last pages and byte-exact captured data. They also cover the captured 2.67-GiB range without allocating that amount in the test. Android launch tests verify that the conservative indirect/readback options are isolated by confirmed title ID. Native shader, clock, ABI and packaging checks remain required. Device memory peak, gameplay progression and rendering must be checked with the resulting APK on the user device.
