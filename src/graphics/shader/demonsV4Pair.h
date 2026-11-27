#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
namespace Libs::Graphics::DemonsV4LightTrace {
struct Pixel {uint32_t lane=0,x=0,y=0,word=0,stencil=0;double luminance=0;};
struct Pair {bool valid=false,contrast=false;uint32_t wg=0,tile=0;Pixel dark{},control{};};
inline double Half(uint16_t v){
    const uint32_t e=(v>>10)&31,m=v&1023;
    if(e==31)return std::numeric_limits<double>::quiet_NaN();
    const double a=e?std::ldexp(double(1024+m),int(e)-25):std::ldexp(double(m),-24);
    return v&0x8000?-a:a;
}
// Both candidates must actually be queued, admitted, and have nonzero packed
// material RGB. Prefer the same stencil/category and a truly dark/bright
// incoming HDR pair; fallback preserves the strongest relative contrast.
inline Pair SelectPair(std::span<const uint32_t> tiles,uint32_t w,uint32_t h,
    std::span<const uint32_t> material,std::span<const uint8_t> stencil,
    std::span<const uint16_t> hdr){
    Pair best;const uint64_t n=uint64_t(w)*h;
    if(!n||material.size()!=n||stencil.size()!=n||hdr.size()!=n*4||tiles.empty())return best;
    const uint32_t cols=(w+7)/8,rows=(h+7)/8;
    double best_delta=-1,best_dark=0;uint32_t best_distance=0;
    for(uint32_t k=0;k<tiles.size();++k){
        const auto t=tiles[k];if(t>=uint64_t(cols)*rows)return {};
        std::array<Pixel,64> points{};uint32_t count=0;
        for(uint32_t l=0;l<64;++l){
            const uint32_t x=(t%cols)*8+l%8,y=(t/cols)*8+l/8;
            if(x>=w||y>=h)continue;
            const uint64_t i=uint64_t(y)*w+x;
            if(!(material[i]&0xffffff)||!(stencil[i]&64))continue;
            const auto* p=hdr.data()+4*i;
            const double lum=.2126*Half(p[0])+.7152*Half(p[1])+.0722*Half(p[2]);
            if(!std::isfinite(lum)||lum<0)continue;
            points[count++]={l,x,y,material[i],stencil[i],lum};
        }
        for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<count;++j){
            if(i==j)continue;
            const auto& d=points[i];const auto& c=points[j];
            if(d.stencil!=c.stencil||d.luminance>=.003||c.luminance<d.luminance)continue;
            const bool contrast=c.luminance>=.006;
            const double delta=c.luminance-d.luminance;
            const int dx=int(d.x)-int(c.x),dy=int(d.y)-int(c.y);const uint32_t dist=dx*dx+dy*dy;
            if(!best.valid||(contrast&&!best.contrast)||(contrast==best.contrast&&
                (delta>best_delta||(delta==best_delta&&(d.luminance<best_dark||
                (d.luminance==best_dark&&dist<best_distance)))))){
                best={true,contrast,k,t,d,c};best_delta=delta;best_dark=d.luminance;best_distance=dist;
            }
        }
    }
    return best;
}
}
