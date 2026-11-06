# Sparse backing uploads and Demon’s Souls regression rollback

The b4713c5 APK still receives Android exit reason 3 (low memory) in Wolverine,
PID 26237, diagnostics 20261106-171339. The 2,868,903,936-byte finite SSBO passes
the segmented descriptor path. Bounded staging uploads alone did not prevent OOM.

Large buffer uploads previously memcpy-read every mapped tmpfs backing page. On
Linux, mmap read faults populate even untouched tmpfs holes. Thus reading the
whole arena can allocate guest backing in addition to its Vulkan mirror; limiting
staging buffers does not prevent this. A local 128 MiB memfd reproduction with one
written page grew backing allocation from 4 KiB to 128 MiB with mapped reads;
pread retained 4 KiB and returned identical zero-filled holes.

For sparse transfers >=1 MiB, GuestBackingStore now reads each mapped backing span
using SEEK_DATA/SEEK_HOLE extents and pread only for actual data, handles EINTR/short
reads, and reports read errors rather than falling
back to mapped reads. Written MAP_SHARED bytes and physical aliases retain their
file offsets. Reserved holes remain zero; private mappings retain the original
copy path. Smaller reads and Windows retain memcpy. Kernels without extent seeks fall back
to pread. No hole map is cached across transfers, so later guest writes remain visible. This preserves full shader
buffer bounds and the existing 32 MiB staging window; it does not eliminate the
Vulkan mirror or guarantee the game fits the device’s memory.

The production memory regression test copies a 64 MiB mapped arena with three
written bytes, checks every output byte and canaries, repeats an unaligned read,
and checks memfd allocated block counts do not increase. The old mapped-read path
fails this allocation assertion even though content comparison passes.

Demon’s Souls PID 25925 exits at renderCompute.cpp:483 (invalid command buffer),
and PID 26068 at bufferCache.cpp:1130 (no recording command buffer). Both launched
with the newly forced gpu-indirect=0 and targeted-readback=0. Remove both title
overrides together to restore the prior launch path. This rollback does not claim
the scenery is fixed or isolate which override exposed the scheduler failure.
The previously working pointer-tagging/Bink and native-TSC fixes, synchronous
shader compilation, and rebind-after-join remain.

Incremental GitHub caching remains enabled. Native memory changes must invalidate
the complete core cache; unchanged FEX integration and dependencies can be reused.
Android runtime outcomes still require testing the APK on the affected device.

The first full-emulator run (83b16fe5) failed the block-count assertion; a follow-up
with additional host diagnostics (603bf392) passed the entire memory and shader
suites. The cause of that difference was not established. The final path explicitly
skips file holes instead of depending only on pread behavior, and adds a standalone
production-header test for complete content, offsets beyond 4 GiB, unaligned
transfers, concurrent readers, invalid ranges and later MAP_SHARED writes. The
full-emulator allocation assertion remains unchanged and must pass before APK build.
