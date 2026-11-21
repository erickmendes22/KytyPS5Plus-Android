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

// Two full D32/S8 *depth* render surfaces may overlap within a guest heap
// despite separate physical base addresses. They are not two aspects of the
// same native VkImage: aspect association is already represented by depth_id,
// with a separate S8 bookkeeping image. This policy is called only when the
// caller has proved BOTH descriptors are complete depth images (not S8 views).
// Reuse the same strict no-mip/no-array geometry predicate as the color rule.
[[nodiscard]] constexpr bool KeepDistinctDepth(Surface requested,
                                               Surface cached,
                                               bool independent_depth_images) noexcept {
    if (!independent_depth_images || !requested.depth_or_stencil ||
        !cached.depth_or_stencil) return false;
    // A full D32 depth texture may have different byte spans because
    // the owner includes alignment/metadata padding. The equal-size
    // condition of KeepDistinct (for color) is too strict here: it let
    // PPSA01341 select 0x282b60000 for the guest depth 0x2831e0000.
    // Equal native geometry and separate base addresses are sufficient:
    // these are complete one-mip/one-layer depth images, not S8 planes.
    return requested.address != cached.address &&
           requested.size != 0 && cached.size != 0 &&
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

// A full sampled R32 depth view is not itself a Vulkan depth attachment.
// Its pixel format differs from the D32/S8 owner, so KeepDistinctDepth alone
// cannot protect it. If both guest surfaces have complete, single-subresource
// geometry but DIFFERENT base addresses, the sampled view cannot legally
// become the other depth target merely because their padded heaps overlap.
// The caller must independently verify a supported depth/raw-format pair
// and identical element sizes; equal-address reinterpretation stays valid.
[[nodiscard]] constexpr bool KeepDistinctSampledDepth(const Surface& sampled,
                                                       const Surface& depth_owner) noexcept {
    return !sampled.depth_or_stencil && depth_owner.depth_or_stencil &&
           sampled.address != depth_owner.address &&
           sampled.size != 0 && depth_owner.size != 0 &&
           sampled.width == depth_owner.width &&
           sampled.height == depth_owner.height &&
           sampled.depth == depth_owner.depth &&
           sampled.levels == 1u && depth_owner.levels == 1u &&
           sampled.layers == 1u && depth_owner.layers == 1u &&
           sampled.type == depth_owner.type &&
           sampled.samples == depth_owner.samples &&
           sampled.tile == depth_owner.tile;
}

} // namespace Libs::Graphics::ViewPlacementPolicy
