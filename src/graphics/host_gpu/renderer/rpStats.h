#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RPSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RPSTATS_H_

#include <cstdint>
#include <string>

// KYTY_RP_STATS=1 (opt-in diagnostics, with --perf-stats): how many guest colour clears
// (TextureCache::ClearImage, full image) reach a render pass that uses the image as a colour
// attachment over its whole area before anything else touches it - the clears a loadOp CLEAR
// could absorb - and how many megabytes those attachments hold. "[perf-wait] rp_clears:".
namespace Libs::Graphics::RpStats {

[[nodiscard]] bool Enabled();
void OnClear(bool full_color);
// A pending clear met a render pass: `full_area` whether the pass covers the whole image.
void OnAttachment(bool full_area, uint64_t bytes);
// KYTY_RP_OPS (live rp_ops=0/1): fold full colour clears into the next full-area pass.
[[nodiscard]] bool FoldEnabled();
void OnFolded();      // a pass absorbed a clear as loadOp CLEAR
void OnFoldFlushed(); // another use recorded the clear first
void Append(std::string& out, double per_frame);

} // namespace Libs::Graphics::RpStats

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RPSTATS_H_ */
