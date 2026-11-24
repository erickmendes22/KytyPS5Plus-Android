#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <algorithm>

namespace Libs::Graphics::DemonsGeometryExecPolicy {
// Identical native wave64 NGG prologue in 093025's four depth/material VS.
// s0 = s3 & 0xff; s106 = 64 - s0; s_lshr_b64 exec,-1,s106 at 0x3c.
// Vulkan vertex invocations need not occupy a contiguous prefix of lanes.
// Restore the ballot of the actual running invocations at the ES entry,
// not the earlier GS/primitive-export EXEC, and never change other code.
constexpr std::array<uint32_t,16> Prefix {
    0xbfa00003,0x93ebff03,0x00080008,0x8700ff03,
    0x000000ff,0x8f6a8c6b,0x887c6a00,0xbf800000,
    0xbf900009,0x81ea6bc0,0x90fe6ac1,0xf8000941,
    0x00000000,0x81ea00c0,0xbf8cff0f,0x90fe6ac1
};
constexpr bool Watched(uint64_t h) {
    return h==0x34c51585c926a664ull || h==0xa7208481a1f2b760ull ||
           h==0xf825a988127faeb4ull || h==0x74b55dea9562eda1ull;
}
constexpr bool Applies(bool enabled,uint64_t hash,uint32_t wave,std::span<const uint32_t> code) {
    return enabled && wave==64 && Watched(hash) && code.size()>=Prefix.size() &&
           std::equal(Prefix.begin(),Prefix.end(),code.begin());
}
inline bool Enabled() {
    const char* v=std::getenv("KYTY_DS_NGG_LIVE_EXEC");
    return v && v[0]=='1' && v[1]=='\0';
}
constexpr uint64_t DrawBuilder = 0xf0c140ea8df8f532ull;
constexpr uint32_t SourceIndex(uint32_t lane,uint32_t draw) { return lane*22u+draw; }
constexpr uint32_t DestIndex(uint32_t lane,uint32_t draw) { return lane*168u+draw*8u+1u; }
} // namespace Libs::Graphics::DemonsGeometryExecPolicy
