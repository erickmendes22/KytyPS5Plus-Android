#pragma once

#include <cstdint>

namespace Libs::Graphics {

// Vulkan's per-attachment write enable cannot express per-component masks.
constexpr uint32_t GuestColorWriteMask(uint32_t target_mask, uint32_t shader_mask,
                                       uint32_t slot) noexcept {
    if (slot >= 8u) return 0;
    return ((target_mask & shader_mask) >> (slot * 4u)) & 0x0fu;
}

constexpr bool GuestColorWriteEnabled(uint32_t target_mask, uint32_t shader_mask,
                                      uint32_t slot, bool has_image) noexcept {
    return has_image && GuestColorWriteMask(target_mask, shader_mask, slot) != 0u;
}

} // namespace Libs::Graphics
