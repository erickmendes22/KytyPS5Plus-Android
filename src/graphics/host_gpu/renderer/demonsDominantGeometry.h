#pragma once
#include <cstdint>

namespace Libs::Graphics::DemonsDominantGeometry {
constexpr uint64_t DepthAddress = 0x282b60000ull;
constexpr uint64_t OccluderVs = 0x2337192397a30339ull;
constexpr uint64_t DepthPs = 0x651f53f5932d8794ull;
constexpr uint64_t DominantDepthVs = 0xaff9c7ef505c6253ull;
constexpr uint64_t MaterialVs = 0xc96f3fcde8141266ull;
constexpr uint64_t MaterialPs = 0x552d823dcfcf2deeull;
constexpr bool Watched(uint64_t hash) {
    return hash == OccluderVs || hash == DepthPs || hash == DominantDepthVs ||
           hash == MaterialVs || hash == MaterialPs ||
           // Actual zero-coverage depth/material geometry from the 213553 capture.
           hash == 0x34c51585c926a664ull || hash == 0xa7208481a1f2b760ull ||
           hash == 0x3dce2bd78d58db62ull || hash == 0x895c9fe037e704f5ull ||
           hash == 0xf825a988127faeb4ull || hash == 0x74b55dea9562eda1ull ||
           hash == 0xcfc1d9193e7f9e53ull || hash == 0xc92deb23fc611b10ull;
}
// Optional isolation: frame 497 measured two unmatched depth writers,
// not proof that either is invalid. Preserve clear, stencil and comparison.
constexpr bool SuppressDepthWrite(bool enabled, uint64_t vs, uint64_t ps,
                                  uint64_t depth, bool gbuffer, bool direct,
                                  bool writes_depth, uint32_t indices, uint32_t instances,
                                  uint32_t compare) {
    return enabled && vs == OccluderVs && ps == DepthPs && depth == DepthAddress &&
           !gbuffer && direct && writes_depth && instances == 1 && compare == 6 &&
           (indices == 7296 || indices == 192);
}
} // namespace Libs::Graphics::DemonsDominantGeometry
