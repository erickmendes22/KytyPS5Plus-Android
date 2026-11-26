#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace Libs::Graphics::DemonsDominantGeometry {
constexpr uint64_t DepthAddress = 0x282b60000ull;
constexpr uint64_t OccluderVs = 0x2337192397a30339ull;
constexpr uint64_t DepthPs = 0x651f53f5932d8794ull;
constexpr uint64_t DominantDepthVs = 0xaff9c7ef505c6253ull;
constexpr uint64_t MaterialVs = 0xc96f3fcde8141266ull;
constexpr uint64_t MaterialPs = 0x552d823dcfcf2deeull;
// The exact instance-count fields at the five records observed in three
// independent sessions (211426, 213553, 224957). No generated values.
constexpr std::array<uint64_t,5> IndirectRecords {
    0x20934d2a0ull, 0x20934d540ull, 0x20934d7e0ull, 0x20934da80ull, 0x20934dd20ull
};
constexpr bool Overlaps(uint64_t address, uint64_t bytes, uint64_t target, uint64_t length) {
    return bytes && length && (address <= target ? target-address < bytes : address-target < length);
}
constexpr bool CountOverlap(uint64_t address, uint64_t bytes) {
    for (auto site: IndirectRecords) if (Overlaps(address,bytes,site+4,4)) return true;
    return false;
}
// Verified from the native f0c140ea8df8f532 load at 0x7c and
// SPIR-V binding 0 array element 2 (frame 417/473/474/665).
constexpr uint64_t CountInputAddress = 0x2093634c0ull;
constexpr uint32_t CountInputWords = 2816;
constexpr uint32_t CountInputSlot = 2;
constexpr uint32_t CountOutputSlot = 3;
constexpr uint32_t CountSourceIndex(uint32_t lane, uint32_t draw) { return 22u*lane+draw; }
constexpr uint32_t CountDestIndex(uint32_t lane, uint32_t draw) { return 168u*lane+8u*draw+1u; }
constexpr bool CountInputOverlap(uint64_t address, uint64_t bytes) {
    return Overlaps(address,bytes,CountInputAddress,CountInputWords*4u);
}
static_assert(CountSourceIndex(2,0)==44 && CountDestIndex(2,0)==337);
static_assert(CountSourceIndex(63,20)<CountInputWords);
// First actual scene-depth writers in #721, before all 41 zero-coverage
// material draws. Retain the two earlier small-mesh controls.
constexpr std::array<uint64_t,7> DepthProbeShaders{
    0x3dce2bd78d58db62ull,0x895c9fe037e704f5ull,
    0x6254880342eb8db4ull,0x31e660625354d953ull,
    0x3cc41a5737b15e7full,0x9da9c10e59d6e38eull,0xe1638ea46c70bc9cull
};
constexpr std::size_t DepthProbeSlot(uint64_t hash) {
    for(std::size_t i=0;i<DepthProbeShaders.size();++i)
        if(DepthProbeShaders[i]==hash)return i;
    return DepthProbeShaders.size();
}
constexpr bool ProbeDepthPs(uint64_t hash) {
    return DepthProbeSlot(hash)<DepthProbeShaders.size();
}
static_assert(DepthProbeSlot(0x6254880342eb8db4ull)==2);
static_assert(DepthProbeSlot(0x31e660625354d953ull)==3);
static_assert(!ProbeDepthPs(0x651f53f5932d8794ull));
constexpr bool WatchedIndirectVs(uint64_t hash) {
    return hash == 0x7b3be1892b062c73ull || hash == 0x6a9c615b413ad6bfull ||
           hash == 0xa9ef1305dc9f9047ull || hash == 0x04a68bcda1467ed2ull ||
           hash == 0x1b14b4883aece7ebull;
}
constexpr bool Watched(uint64_t hash) {
    return ProbeDepthPs(hash) || hash == 0x868546d85ce5090aull || hash == 0x0a472b34f593b99bull ||
           hash == OccluderVs || hash == DepthPs || hash == DominantDepthVs ||
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
