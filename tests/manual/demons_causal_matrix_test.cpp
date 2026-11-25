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
}
