#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include <cassert>
using namespace Libs::Graphics::DemonsCausalMatrix;
int main(){
    assert(Parse(nullptr)==Control&&Parse("")==Control&&Parse("9")==Control&&Parse("71")==Control);
    assert(Parse("3")==CpCoherent&&Parse("4")==GpuBarrier&&Parse("6")==PassOrder&&Parse("8")==Trace);
    static_assert(CountExpected(0,0,9,0)==std::array<uint32_t,5>{1,1,1,0,1});
    static_assert(CountExpected(100,4,64,3)==std::array<uint32_t,5>{3,1,1,192,3});
    static_assert(CountExpected(64,0,64,5)==std::array<uint32_t,5>{1,1,1,0,0});
    static_assert(OrderedShader(0xfb18d110fab8234eull)&&!OrderedShader(0));
    // #719, frame 503, captured SRT and sparse input tables. The previous
    // oracle transposed slots 14/15, reading secondary[100] instead of [55].
    std::array<uint32_t,18> srt{};srt[14]=100;srt[15]=55;srt[16]=66;srt[17]=200;
    std::array<uint32_t,1152> primary{},secondary{};
    primary[66]=12;secondary[55]=81;secondary[100]=0;
    assert(HasSecondary(srt));
    const auto expected=ReplayCount(srt,primary,secondary);
    assert(expected && (*expected==std::array<uint32_t,5>{16,1,1,972,81}));
    srt[14]=0;srt[15]=990;
    const auto no_secondary=ReplayCount(srt,primary,{});
    assert(!HasSecondary(srt)&&no_secondary&&(*no_secondary==std::array<uint32_t,5>{1,1,1,12,1}));
    srt[14]=1;srt[15]=0;secondary[0]=1;
    assert(HasSecondary(srt)&&ReplayCount(srt,primary,secondary)==no_secondary);
    srt[15]=1152;assert(!ReplayCount(srt,primary,secondary));
    static_assert(LightingQueue(0xf0c482b0b355e9deull)==0&&LightingQueue(0x3dfe402653925fbfull)==8&&LightingQueue(0xe3daeb1e7426761bull)==33);
}
