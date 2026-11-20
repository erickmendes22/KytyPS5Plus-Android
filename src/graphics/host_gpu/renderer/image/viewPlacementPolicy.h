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

// Requested and cached Vulkan images cannot represent the same single
// native mip/layer when their guest base addresses differ. Even if their
// guest ranges overlap (a packed deferred-renderer heap), neither is a
// subresource of the other and merging them would alias two G-buffers.
struct Surface {
    uint64_t address = 0, size = 0;
    uint32_t width = 0, height = 0, depth = 0;
    uint32_t levels = 0, layers = 0;
    uint32_t format = 0, type = 0, samples = 0, tile = 0;
    bool depth_or_stencil = false;
};

[[nodiscard]] constexpr bool KeepDistinct(const Surface& requested,
                                           const Surface& cached) noexcept {
    // Depth and stencil legitimately use different guest-plane addresses but
    // share one D32S8 Vulkan image. Never split that association.
    return !requested.depth_or_stencil && !cached.depth_or_stencil &&
           requested.address != cached.address &&
           requested.size != 0 && requested.size == cached.size &&
           requested.width == cached.width &&
           requested.height == cached.height &&
           requested.depth == cached.depth &&
           requested.levels == 1u && cached.levels == 1u &&
           requested.layers == 1u && cached.layers == 1u &&
           requested.format == cached.format &&
           requested.type == cached.type &&
           requested.samples == cached.samples &&
           requested.tile == cached.tile;
}

} // namespace Libs::Graphics::ViewPlacementPolicy
