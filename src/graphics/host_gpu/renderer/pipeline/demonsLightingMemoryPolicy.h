#pragma once

#include <cstdint>

// Shader hashes are taken from the six Demon's Souls PPSA01341 lighting
// dispatches and the scene composite reported by the Android 50fb3da1 logs.
// Never turn on CPU/GPU synchronization in unrelated games or shader stages.
namespace Libs::Graphics::DemonsLightingMemoryPolicy {

[[nodiscard]] constexpr bool NeedsCoherentScalarReads(uint64_t hash) noexcept {
    switch (hash) {
        case 0xc2e01fbe98f5ebabull:
        case 0x23056085c1120461ull:
        case 0x3dfe402653925fbfull:
        case 0xfabe0bcb275a3005ull:
        case 0xe75589aa0bb3ae52ull:
        case 0x93b0be7f1a0293d1ull:
        case 0x3265b01ed00a5c01ull: return true;
        default: return false;
    }
}

// Previously recognized only seven captured hash variants. Revalidate all
// sampled compute light passes and multi-input pixel composites for the
// title-specific stable-view path. Other games retain their normal cache.
[[nodiscard]] constexpr bool NeedsStableSampledImages(uint64_t hash, bool compute,
                                                       bool pixel, uint32_t sampled_images) noexcept {
    return NeedsCoherentScalarReads(hash) ||
           (compute && sampled_images != 0u) ||
           (pixel && sampled_images >= 2u);
}

// Existing SPIR-V software BVH reads GPU-built indirect SRT/BVH buffers.
// A producer barrier is necessary; never silently stub out ray intersections.
[[nodiscard]] constexpr bool NeedsBvhProducerBarrier(bool demons_title,
                                                     bool bvh_program) noexcept {
    return demons_title && bvh_program;
}
} // namespace Libs::Graphics::DemonsLightingMemoryPolicy
