#pragma once

#include <cstdint>

namespace Libs::Graphics {

// A guest raster mask describes CB writes independently from the translated
// pixel shader's declared MRT exports. When the latter misses an export,
// using only the translated list silently drops a live color attachment.
//
// No target is invented: eligible_mask must contain ONLY slots whose guest
// CB registers have both an enabled write mask and a nonzero backing address.
// Scope invocation to verified game compatibility paths (not all titles).
[[nodiscard]] constexpr uint32_t RecoverGuestColorTargets(
    uint32_t translated_mask, uint32_t guest_export_mask,
    uint32_t eligible_mask) noexcept {
    return translated_mask | (guest_export_mask & eligible_mask);
}

} // namespace Libs::Graphics
