#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
constexpr std::array<uint32_t,5> CountExpected(uint32_t primary,uint32_t secondary,uint32_t limit,uint32_t secondary_limit) {
    const uint32_t n=secondary_limit?std::min(secondary,secondary_limit):1u;
    const uint32_t count=std::min(primary,limit)*n;
    return {std::max(1u,(count+63u)>>6u),1u,1u,count,n};
}
}
