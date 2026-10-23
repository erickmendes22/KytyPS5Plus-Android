#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_UPLOADSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_UPLOADSTATS_H_

#include <cstdint>
#include <string>

// KYTY_UPLOAD_STATS=1 (opt-in, with --perf-stats; diagnostics): "[perf-wait] stream:" - the
// BufferCache small-read stream path (CPU-modified ranges <= 16 KiB copied into the stream buffer
// on every use): calls, KiB and copy time per frame, a size histogram, and how much of it re-sent
// bytes that had not changed since the same range was last streamed (content hash per range).
// "sync:" - SynchronizeBuffer uploads (calls, KiB). Any thread may note; Append on the reporter.
namespace Libs::Graphics::UploadStats {

[[nodiscard]] bool Enabled();
void NoteStream(uint64_t vaddr, uint64_t size, const void* data, uint64_t copy_ns);
void NoteSync(uint64_t bytes);
// KYTY_STREAM_PROMOTE promotions (always counted; "stream_promoted=" in the report when non-zero).
void NotePromotion();
void Append(std::string& out, double per_frame);

} // namespace Libs::Graphics::UploadStats

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_UPLOADSTATS_H_ */
