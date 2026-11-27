#include "graphics/shader/demonsV4Pair.h"
#include <cassert>
using namespace Libs::Graphics::DemonsV4LightTrace;
int main(){
    std::array<uint32_t,1> tiles{0};
    std::array<uint32_t,64> m{};
    std::array<uint8_t,64> s{};
    std::array<uint16_t,256> h{};
    m[0]=m[1]=m[2]=m[3]=0xff123456;
    s[0]=s[1]=86;s[2]=0;s[3]=90;
    h[4]=0x3400;h[8]=h[12]=0x3c00;
    auto p=SelectPair(tiles,8,8,m,s,h);
    assert(p.valid&&p.contrast&&p.dark.lane==0&&p.control.lane==1);
    h[4]=0;p=SelectPair(tiles,8,8,m,s,h);assert(p.valid&&!p.contrast);
    s[1]=90;s[3]=0;p=SelectPair(tiles,8,8,m,s,h);assert(!p.valid);
    tiles[0]=1;assert(!SelectPair(tiles,8,8,m,s,h).valid);
}
