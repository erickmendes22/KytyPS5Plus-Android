#pragma once
#include "graphics/host_gpu/renderer/htileLoadPolicy.h"
#include <cstdint>

namespace Libs::Graphics {
// Only one-layer, single-mip sampled depth images are covered. An HTile fill
// can describe a depth clear that was never materialized on the Vulkan image
// because the surface has been sampled, not acquired as a depth target.
struct SampleHtileDecision {
    bool clear_depth = false;
    bool keep_stencil_meta = false;
    float clear_depth_value = 0.0f;
};
inline constexpr SampleHtileDecision DecideSampleHtile(bool pending, bool fill_known,
    uint32_t fill, bool stencil_compressed) {
    if (!pending || !fill_known ||
        (!HtileFillClearsDepth(fill) && !HtileFillDepthUniform(fill, stencil_compressed)))
        return {};
    const auto d = DecideHtileLoad(pending, fill, fill_known, stencil_compressed);
    return {d.depth_meta_clear, d.stencil_meta_clear,
            HtileFillDepthValue(fill, stencil_compressed)};
}
} // namespace Libs::Graphics
