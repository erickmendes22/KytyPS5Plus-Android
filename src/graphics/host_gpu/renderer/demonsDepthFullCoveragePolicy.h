#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace Libs::Graphics {
struct DepthFullCoverage {
    bool valid = false;
    uint64_t changed = 0, filled_before = 0, filled_after = 0;
    uint64_t far_before = 0, far_after = 0;
    uint32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};
inline constexpr bool HasDepth(uint32_t bits) {
    // D32_SFLOAT reversed Z: depth=0 is the cleared far plane.
    // Exclude NaN/Inf data, which is not usable scene depth.
    return (bits & 0x7fffffffu) != 0 && (bits & 0x7f800000u) != 0x7f800000u;
}
inline DepthFullCoverage CompareDepthFull(std::span<const uint32_t> before,
                                          std::span<const uint32_t> after,
                                          uint32_t width, uint32_t height) {
    DepthFullCoverage d;
    const uint64_t area = uint64_t{width} * height;
    if (!width || !height || area > 4ull * 1024u * 1024u ||
        before.size() != area || after.size() != area) return d;
    d.valid = true;
    for (uint64_t i = 0; i < area; ++i) {
        d.filled_before += HasDepth(before[i]);
        d.filled_after += HasDepth(after[i]);
        d.far_before += before[i] == 0u;
        d.far_after += after[i] == 0u;
        if (before[i] != after[i]) {
            const auto x = static_cast<uint32_t>(i % width);
            const auto y = static_cast<uint32_t>(i / width);
            if (d.changed == 0) { d.min_x = d.max_x = x; d.min_y = d.max_y = y; }
            else {
                d.min_x = std::min(d.min_x, x); d.max_x = std::max(d.max_x, x);
                d.min_y = std::min(d.min_y, y); d.max_y = std::max(d.max_y, y);
            }
            ++d.changed;
        }
    }
    return d;
}
} // namespace Libs::Graphics
