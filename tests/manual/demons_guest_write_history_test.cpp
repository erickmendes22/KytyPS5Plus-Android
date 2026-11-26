#include "cpu/demonsGuestWriteHistory.h"
#include <cassert>
namespace H=Cpu::DemonsGuestWriteHistory;
int main(){
    uint64_t gpr[16]{};gpr[3]=0xdeadbeef;
    H::Evidence e{};e.words[14]=4096;e.words[15]=0x12345678;
    H::Record(0x11010,0x900001,0x22000,0x11010,512,gpr,&e);
    assert(H::VertexCandidate(0x3a8200100ull));
    assert(!H::VertexCandidate(0x3a8220000ull));
    H::Record(0x24000,0x900002,0,0,0);
    auto x=H::Overlap(0x11800,2048);
    assert(x.events.size()==1&&x.events[0].rip==0x900001&&x.events[0].rsi==0x22000);
    assert(x.events[0].gpr[3]==0xdeadbeef&&x.newest==2&&x.oldest==1);
    assert(x.events[0].evidence.words[14]==4096&&x.events[0].evidence.words[15]==0x12345678);
    assert(H::Overlap(0x30000,2048).events.empty());
}
