#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <optional>
namespace Libs::Graphics::DemonsCausalMatrix {
enum ModeId : unsigned { Control=0, IndexCull=1, ShadowCull=2, CpCoherent=3, GpuBarrier=4,
                        FrameOrder=5, PassOrder=6, Replay=7, Trace=8 };
inline ModeId Parse(const char* s) {
    return s && s[0]>='0' && s[0]<='8' && s[1]=='\0' ? static_cast<ModeId>(s[0]-'0') : Control;
}
inline ModeId Mode() { static const auto m=Parse(std::getenv("KYTY_DS_MATRIX")); return m; }
inline bool Evidence() { return Mode()==Replay || Mode()==Trace; }
constexpr bool OrderedShader(uint64_t hash) {
    return hash==0xcb3b9c1ea64fe876ull || hash==0xfb18d110fab8234eull ||
        hash==0xf0c140ea8df8f532ull || hash==0xbcbcc6ba1e68c974ull ||
        hash==0x567d21ad45386c8dull || hash==0xc03afccb96a13c5dull ||
        hash==0xacce347eef557bd2ull || hash==0xe3daeb1e7426761bull;
}
// Original AMDGPU scalar loads, not a guessed flattened SRT ordering:
// 0x10 limit (root+0xc), 0x28 index (root+0x4); primary index/limit
// 0x54 and 0x78. Certified by the live three-descriptor trace.
constexpr size_t SecondaryLimit=14, SecondaryIndex=15, PrimaryIndex=16, PrimaryLimit=17;
constexpr bool HasSecondary(std::span<const uint32_t> srt) {
    return srt.size()==18 && srt[SecondaryLimit]!=0;
}
constexpr std::array<uint32_t,5> CountExpected(uint32_t primary,uint32_t secondary,uint32_t limit,uint32_t secondary_limit) {
    const uint32_t n=secondary_limit?std::min(secondary,secondary_limit):1u;
    const uint32_t count=std::min(primary,limit)*n;
    return {std::max(1u,(count+63u)>>6u),1u,1u,count,n};
}
constexpr std::optional<std::array<uint32_t,5>> ReplayCount(
    std::span<const uint32_t> srt,std::span<const uint32_t> primary,std::span<const uint32_t> secondary) {
    if(srt.size()!=18 || srt[PrimaryIndex]>=primary.size() ||
        (HasSecondary(srt)&&srt[SecondaryIndex]>=secondary.size()))return {};
    return CountExpected(primary[srt[PrimaryIndex]],
        HasSecondary(srt)?secondary[srt[SecondaryIndex]]:0u,srt[PrimaryLimit],srt[SecondaryLimit]);
}
constexpr uint32_t LightingQueue(uint64_t hash) {
    return hash==0xf0c482b0b355e9deull?0u:hash==0x3dfe402653925fbfull?8u:
        hash==0xe3daeb1e7426761bull?33u:UINT32_MAX;
}
}
