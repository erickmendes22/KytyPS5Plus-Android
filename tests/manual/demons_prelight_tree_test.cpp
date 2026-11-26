#include "graphics/host_gpu/renderer/demonsPrelightPolicy.h"
#include <array>
#include <cassert>
using namespace Libs::Graphics::DemonsPrelightPolicy;
int main(){
    constexpr std::array<uint32_t,4> a{0,0xff000000u,0x1234u,0x1235u},b{1,0,0,0x1236u};
    auto d=Compare(a,b);
    assert(d.valid&&d.changed==4&&d.before==2&&d.after==2&&d.lost==1&&d.gained==1);
    assert(!Compare(a,std::span<const uint32_t>{}).valid);
    static_assert(Overlaps(99,2,100,4)&&!Overlaps(99,1,100,4));
    static_assert(!Overlaps(100,0,100,1));
    static_assert(Decide(false,true,true,true,false)==Root::Blocked);
    static_assert(Decide(true,false,true,false,false)==Root::Blocked);
    static_assert(Decide(true,true,false,true,false)==Root::Blocked);
    static_assert(Decide(true,true,true,true,true)==Root::Blocked);
    static_assert(Decide(true,true,true,true,false)==Root::Produced);
    static_assert(Decide(true,true,true,false,false)==Root::NotProduced);
    ProductionWindow w;
    w.Observe(5,0,1440000); assert(w.Phase(5)==-1);
    w.Observe(6,1440000,1440000);w.Observe(7,655,1440000);assert(w.Phase(7)==1&&w.Phase(12)==1&&w.Phase(13)==-1);
    w.Observe(7,1440000,1440000);w.Observe(8,0,1440000);assert(w.Phase(8)==1);
    w.Observe(9,1359577,1440000);w.Observe(10,11301,1440000);assert(w.Phase(10)==2);
    w.Observe(11,0,1440000);assert(w.Phase(15)==2&&w.Phase(16)==-1);
}
