#include "graphics/shader/demonsV4LightTrace.h"
#include "graphics/host_gpu/renderer/demonsP8CapturePolicy.h"
#include <cassert>
using namespace Libs::Graphics::DemonsV4LightTrace;
int main(){
    static_assert(LegacyWords*4==1664112);
    static_assert(SingleWords*4==1860720);
    static_assert(PairWords*4==3631296);
    static_assert(BrdfLive.size()==37&&BrdfOps.size()==79&&BrdfStride==198);
    static_assert(Bytes==3986112);
    static_assert(BrdfWitnessWords==224*198);
    static_assert(BrdfOps.front().pc==0x1bb4&&BrdfOps.back().pc==0x1d50);
    for(std::size_t j=1;j<BrdfOps.size();++j)assert(BrdfOps[j-1].pc<BrdfOps[j].pc);
    static_assert(WitnessStride==442644&&TermBase+WitnessStride==SingleWords);
    static_assert(RecordBase==LegacyWords&&RecordStride==48);
    static_assert(LightBufferBytes==86016);
    static_assert(FixedBase==8&&OuterBase==6152&&TermBase==22536);
    static_assert(ImageOffset(0)==ImageBase);
    static_assert(ImageOffset(9)==ImageBase+9*ImageStride);
    static_assert(ImageOffset(10)==ImageOffset(9)+Terms*ImageStride);
    static_assert(Libs::Graphics::DemonsP8CapturePolicy::CriticalRole("q8_hdr_before"));
    static_assert(Libs::Graphics::DemonsP8CapturePolicy::CriticalRole("q8_hdr_after"));
    assert(Supports(8,8,1,64));
    assert(!Supports(8,8,1,32));
    assert(!Supports(16,4,1,64));
    for(std::size_t i=0;i<Images.size();++i){
        assert(Images[i].coord_count<=7&&Images[i].count<=4);
        assert(ImageOffset(i+1)>ImageOffset(i));
    }
}
