#pragma once

#include <cstdint>

namespace Libs::Graphics {

// Chen KytyPS5 backport: binding an MRT does not mean that the PS writes to it.
// The guest target write mask and the pixel shader export mask must both allow
// at least one component before the host may enable color writes.
constexpr bool GuestColorWriteEnabled(uint32_t target_mask, uint32_t shader_mask,
                                      uint32_t slot, bool has_image) noexcept {
    if (!has_image || slot >= 8u) return false;
    const uint32_t shift = slot * 4u;
    return (((target_mask & shader_mask) >> shift) & 0x0fu) != 0u;
}

} // namespace Libs::Graphics
