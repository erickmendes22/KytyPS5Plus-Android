#pragma once
#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics::DemonsPcRenderParity {
// Reference: Jetsku/KytyPS5 dd9f9901, U59 preset PPSA01341.
// This switches only guest-visible renderer rules, not mobile Vulkan features.
inline bool Enabled(){
    static const bool on=[] {const char* v=std::getenv("KYTY_DS_PC_RENDER_PARITY");return v&&v[0]=='1'&&v[1]=='\0';}();
    return on;
}
constexpr bool PixelActive(bool present,uint32_t target,uint32_t shader,bool side_effects){
    return present&&((target&shader)!=0||side_effects);
}
constexpr bool DppSkipSource(bool enabled,bool bound,bool fetch_inactive,bool dpp8){
    return enabled&&!bound&&!fetch_inactive&&!dpp8;
}
struct HtileRange {
    uint32_t first=0,count=0;
    bool exact=false;
};
constexpr HtileRange MetaLayers(uint64_t base,uint64_t bytes,uint32_t layers,
                                uint64_t address,uint64_t size,bool slices,bool reject_oversize){
    if(!layers||!bytes||!size||address<base)return {};
    const auto offset=address-base;
    if(offset>=bytes)return {};
    if(reject_oversize&&offset==0&&size>bytes)return {};
    if(!slices)return offset==0?HtileRange{0,layers,true}:HtileRange{};
    const auto stride=bytes/layers;
    if(!stride||bytes%layers)return offset==0?HtileRange{0,layers,true}:HtileRange{};
    if(offset==0&&(size%stride||size>=bytes))return {0,layers,true};
    if(offset%stride||size%stride||size>bytes-offset)return {};
    return {static_cast<uint32_t>(offset/stride),static_cast<uint32_t>(size/stride),offset==0};
}
}
