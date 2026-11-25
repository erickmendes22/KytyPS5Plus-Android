#include "graphics/host_gpu/renderer/demonsRaySourceHistory.h"
#include <cassert>
using namespace Libs::Graphics::DemonsRaySourceHistory;
int main(){
    std::array<uint32_t,RowWords> a{},b{};
    for(uint32_t i=0;i<256;++i){a[2*i+1]=0x40000000;b[2*i+1]=0x40000000;}
    a[256]=0x12000001;b[0]=0x34000001;
    const auto s=Summarize(a);
    assert(s.colored[0]==0&&s.colored[1]==1&&s.high_nibble[1]==1);
    assert(s.finite_depth[0]==128&&s.finite_depth[1]==128);
    assert(DifferentPairs(a,b)==2&&DifferentPairs(a,a)==0);
    History h;h.Begin(0x1000);Record r;r.frame=205;r.dispatch=1;r.valid=true;r.input=s;r.output=s;
    h.Put(14267,r);assert(h.Find(0x1000,14267)->input.digest==s.digest);
    assert(!h.Find(0x2000,14267));h.Put(14267,r);assert(!h.Find(0x1000,14267));
    r.frame=300;r.dispatch=2;h.Put(14267,r);assert(h.Find(0x1000,14267)->frame==300);
    h.Invalidate();assert(!h.Find(0x1000,14267));
}
