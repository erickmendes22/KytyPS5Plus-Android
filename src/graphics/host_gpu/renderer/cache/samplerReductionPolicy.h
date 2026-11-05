#pragma once
#include <cstdint>

// Vulkan's default weighted-average sampler requires no reduction-mode pNext.
// Min/max reduction may only be requested after the physical-device feature
// has been enabled and is forbidden for comparison sampling.
namespace Libs::Graphics {
[[nodiscard]] constexpr bool UseSamplerMinMaxReduction(bool supported, bool compare, uint32_t filter_mode) {
    return supported && !compare && (filter_mode == 1u || filter_mode == 2u);
}
} // namespace Libs::Graphics
