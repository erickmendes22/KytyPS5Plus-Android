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
    static_assert(!DiagnosticRange(0,64));
    static_assert(!DiagnosticRange(0x1000,0));
    static_assert(!DiagnosticRange(UINT64_MAX-2,4));
    static_assert(DiagnosticRange(0x1000,64));
    static_assert(!DiagnosticSlice(0,UINT64_MAX,0,4));
    static_assert(!DiagnosticSlice(0,64,64,4));
    static_assert(!DiagnosticSlice(UINT64_MAX-3,64,4,4));
    static_assert(!DiagnosticSlice(1,64,0,4));
    static_assert(DiagnosticSlice(0x1000,64,16,48));
    static_assert(Decide(false,true,true,true,false)==Root::Blocked);
    static_assert(Decide(true,false,true,false,false)==Root::Blocked);
    static_assert(Decide(true,true,false,true,false)==Root::Blocked);
    static_assert(Decide(true,true,true,true,true)==Root::Blocked);
    static_assert(Decide(true,true,true,true,false)==Root::Produced);
    static_assert(Decide(true,true,true,false,false)==Root::NotProduced);
    // Actual first-cycle boundaries from #741 and #744. The preparation
    // flag was false in both; only the executed flag was true.
    for(auto ids: {std::array<uint64_t,2>{40367,40979},std::array<uint64_t,2>{39624,40091}}){
        ExecutedCycle c;
        assert(!c.Observe(ids[0],true,false)&&!c.first);
        assert(c.Observe(ids[0],true,true)&&c.first==ids[0]);
        assert(!c.Observe(ids[1]-1,false,true)&&c.first==ids[0]);
        assert(c.Includes(ids[0],ids[1])&&c.Includes(ids[1],ids[1]));
        assert(!c.Includes(ids[0]-1,ids[1])&&!c.Includes(ids[1]+1,ids[1]));
        // Regression: the previous selected cycle exhausted its budget.
        // Preparing the boundary draw must not discard its inputs before
        // its executed clear starts a fresh cycle.
        CycleInputRetention retained;
        const CycleInputRetention::Limits limits{8,8,2};
        assert(retained.Retain(ids[0]-3,"12345678",true,true,limits));
        assert(!retained.Retain(ids[0]-2,"lost",false,true,limits));
        assert(retained.dropped==1&&retained.inputs.size()==1);
        assert(retained.Retain(ids[0],"first",true,true,limits));
        assert(retained.dropped==0&&retained.bytes==5&&retained.inputs.size()==1);
        assert(retained.inputs.front().first==ids[0]&&retained.inputs.front().second=="first");
        assert(!retained.Retain(ids[0]+1,"overflow",false,true,limits));
        assert(retained.inputs.front().first==ids[0]&&retained.dropped==1);
        // A boundary whose own record is oversized remains explicitly incomplete.
        assert(!retained.Retain(ids[1],"too_large",true,true,limits));
        assert(retained.inputs.empty()&&retained.bytes==0&&retained.dropped==1);
    }
    // Binary operands participate in the same admission/reset budget as JSON.
    // A short JSON label cannot bypass the retained-byte cap.
    {
        CycleInputRetention retained;
        const CycleInputRetention::Limits limits{12,12,2};
        assert(retained.Retain(1,"json",true,true,limits,8));
        assert(retained.bytes==12&&retained.costs.front()==12);
        assert(!retained.Retain(2,"x",false,true,limits,1));
        assert(retained.Retain(3,"next",true,true,limits,4));
        assert(retained.bytes==8&&retained.dropped==0&&retained.inputs.size()==1);
        assert(retained.Retain(4,"last",false,false,limits,4));
        assert(retained.bytes==8&&retained.inputs.front().first==4);
        assert(!retained.Retain(5,"x",false,true,limits,UINT64_MAX));
        assert(retained.bytes==8&&retained.dropped==1);
    }
    BisectionWindow root;
    assert(root.Phase(10)==-1);
    assert(!root.Observe(10,0,1440000,1440000)&&root.Phase(11)==0);
    assert(!root.Observe(11,33,0,1440000)); // different queue cannot trigger
    assert(root.Observe(11,0,651,1440000)&&root.Edge()==11&&root.Queue()==0);
    assert(root.Phase(13)==1&&root.Phase(14)==-1&&!root.Closed(15)&&root.Closed(16));
    assert(!root.Observe(20,0,0,1440000));
    for(unsigned cause=1;cause<=16;++cause){
        Interval i;unsigned decisions=0;
        assert(!i.Apply(Certificate::Blocked)&&i.lo==0&&i.hi==16);
        while(i.hi-i.lo>1){const auto m=i.Mid();assert(i.Apply(m<cause?Certificate::Pass:Certificate::Fail));++decisions;}
        assert(i.hi==cause&&decisions==4);
    }
    ProductionWindow w;
    w.Observe(5,0,1440000); assert(w.Phase(5)==-1);
    w.Observe(6,1440000,1440000);w.Observe(7,655,1440000);assert(w.Phase(7)==1&&w.Phase(12)==1&&w.Phase(13)==-1);
    w.Observe(7,1440000,1440000);w.Observe(8,0,1440000);assert(w.Phase(8)==1);
    w.Observe(9,1359577,1440000);w.Observe(10,11301,1440000);assert(w.Phase(10)==2);
    w.Observe(11,0,1440000);assert(w.Phase(15)==2&&w.Phase(16)==-1);
}
