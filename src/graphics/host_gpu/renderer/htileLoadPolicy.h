#pragma once
#include <cstdint>

namespace Libs::Graphics {
inline constexpr bool HtileFillClearsDepth(uint32_t fill) { return (fill & 0xfu) == 0u; }
inline constexpr bool HtileFillClearsStencil(uint32_t fill) { return ((fill >> 8u) & 0x3u) == 0u; }
inline constexpr bool HtileFillDepthUniform(uint32_t fill, bool stencil) {
    if ((fill & 0xfu) == 0xfu) return false;
    if (stencil) return ((fill >> 12u) & 0x3fu) == 0u;
    return ((fill >> 4u) & 0x3fffu) == ((fill >> 18u) & 0x3fffu);
}
inline constexpr float HtileFillDepthValue(uint32_t fill, bool stencil) {
    return static_cast<float>(stencil ? fill >> 18u : (fill >> 4u) & 0x3fffu) / 16383.0f;
}
struct HtileLoadDecision {
    bool depth_meta_clear = false;
    bool stencil_meta_clear = false;
    bool depth_uniform = false;
    float uniform_value = 0.0f;
};
inline constexpr HtileLoadDecision DecideHtileLoad(bool meta_cleared, uint32_t fill,
                                                    bool fill_known, bool stencil) {
    HtileLoadDecision d {};
    d.depth_uniform = meta_cleared && fill_known && !HtileFillClearsDepth(fill) &&
                      HtileFillDepthUniform(fill, stencil);
    d.depth_meta_clear = meta_cleared &&
        (!fill_known || HtileFillClearsDepth(fill) || d.depth_uniform);
    d.stencil_meta_clear = meta_cleared && fill_known && stencil &&
                           HtileFillClearsStencil(fill);
    if (d.depth_uniform) d.uniform_value = HtileFillDepthValue(fill, stencil);
    return d;
}
} // namespace Libs::Graphics
