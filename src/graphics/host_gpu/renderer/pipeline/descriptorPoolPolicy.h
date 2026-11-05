#pragma once
// Vulkan descriptor pools must fit every descriptor *within one layout*, not
// merely contain the descriptor type. Tested independently of Vulkan/Turnip.
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace Libs::Graphics::DescriptorPoolPolicy {
using Counts = std::array<uint32_t, 5>; // storage buffer, uniform buffer, sampled image, storage image, sampler

constexpr bool Fits(const Counts& capacity, const Counts& needs) {
    for (size_t i = 0; i < capacity.size(); ++i) {
        if (capacity[i] < needs[i]) return false;
    }
    return true;
}

// Four complete sets from a previously unseen large layout, while preserving
// the original fixed capacities for ordinary shaders. Widen before multiplying.
constexpr uint32_t Capacity(uint32_t base, uint32_t per_set) {
    const uint64_t requested = static_cast<uint64_t>(per_set) * 4u;
    return static_cast<uint32_t>(std::min<uint64_t>(
        std::max<uint64_t>(base, requested), std::numeric_limits<uint32_t>::max()));
}
} // namespace Libs::Graphics::DescriptorPoolPolicy
