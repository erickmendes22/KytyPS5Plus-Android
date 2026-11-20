#pragma once
#include <cstdint>

namespace Libs::Graphics::ViewPlacementPolicy {

// All offsets and counts must fit the selected native image. Use 64-bit sums
// so a malformed guest descriptor cannot wrap a uint32_t level or layer.
[[nodiscard]] constexpr bool Fits(int32_t place_mip, int32_t place_layer,
                                  uint32_t own_mip, uint32_t own_layer,
                                  uint32_t mip_count, uint32_t layer_count,
                                  uint32_t owner_mips, uint32_t owner_layers) noexcept {
    return place_mip >= 0 && place_layer >= 0 &&
           mip_count != 0 && layer_count != 0 &&
           uint64_t{static_cast<uint32_t>(place_mip)} + own_mip + mip_count <= owner_mips &&
           uint64_t{static_cast<uint32_t>(place_layer)} + own_layer + layer_count <= owner_layers;
}

} // namespace Libs::Graphics::ViewPlacementPolicy
