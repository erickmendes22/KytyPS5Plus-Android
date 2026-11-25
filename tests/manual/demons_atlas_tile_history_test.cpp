#include "graphics/host_gpu/renderer/demonsAtlasTileHistory.h"
#include <cassert>
using namespace Libs::Graphics::DemonsAtlasTileHistory;
int main(){
    constexpr std::array<uint32_t,6> srt{4,8,15,15,511,0};
    static_assert(IndexKey(0xab005123u,srt)==Key(48,32,81));
    static_assert(IndexKey(360u<<8,srt)==Invalid);
    static_assert(Key(256,0,0)==Invalid);
    History h;
    h.Begin(1,3);
    Record r;r.frame=228;r.submit=4;r.version=3;r.coefficients[0]=19;
    const auto a=Key(48,32,81),b=Key(16,0,0);
    h.Put(a,r);
    assert(h.Find(1,3,a)->coefficients[0]==19);
    h.Begin(1,5);
    r.frame=229;r.submit=5;r.version=5;h.Put(b,r);
    assert(h.Find(1,5,a)->frame==228);
    assert(!h.Find(1,4,a));
    h.Put(b,r);assert(h.Find(1,5,b)->ambiguous);
    h.Begin(1,9);assert(!h.Find(1,9,a));
    h.Invalidate();assert(!h.Find(1,9,b));
}
