#include "graphics/host_gpu/renderer/demonsMaterialFullFramePolicy.h"
#include <array>
#include <cassert>
using Libs::Graphics::CompareMaterialFullFrame;
int main(){
 std::array<uint32_t,16> before{},after{};
 before.fill(0xff000000u);after=before;after[10]=0xffaabbccu;
 const auto d=CompareMaterialFullFrame(before,after,4,4);
 assert(d.valid&&d.changed==1&&d.min_x==2&&d.max_x==2&&d.min_y==2&&d.max_y==2);
 assert(d.colored_before==0&&d.colored_after==1);
 assert(!CompareMaterialFullFrame(before,after,2,2).valid);
}
