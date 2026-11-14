#pragma once

#include <cstdint>

namespace Libs::Graphics {

// KytyPS5 canonical T# decoding: field1 bit 29 is reserved; WORD5 bit 25
// enables the mip-stats counter and is a valid guest texture descriptor bit.
constexpr uint32_t CanonicalTextureField1ReservedMask = 1u << 29u;
constexpr uint32_t CanonicalTextureField5(uint32_t perf_mod, uint32_t max_mip,
                                          bool mip_stats_enabled) noexcept {
    return (perf_mod << 20u) | (uint32_t {mip_stats_enabled} << 25u) |
           (max_mip << 4u);
}

// WORD6[7:0] is a mip-stats counter, not part of the HTILE control bits.
// Uncompressed depth sampling ignores HTILE metadata (COMPRESSION_EN == 0).
constexpr bool IsSupportedDepthMetadataControl(uint32_t word6, bool compression_enabled,
                                               bool msaa_depth) {
    if (!compression_enabled) {
        return true;
    }
    constexpr uint32_t kMetadataControlMask = 0x00ffff00u;
    constexpr uint32_t kHtileControl = (1u << 19u) | (1u << 21u);
    const uint32_t expected = kHtileControl | (msaa_depth ? (1u << 10u) : 0u);
    return (word6 & kMetadataControlMask) == expected;
}

} // namespace Libs::Graphics
