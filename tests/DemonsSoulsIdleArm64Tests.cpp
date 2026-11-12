#include "loader/demonsSoulsIdle.h"
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#if defined(__x86_64__) && !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif
using namespace Loader::DemonsSoulsIdle;
namespace {
std::atomic<int> waits{0};
extern "C" void HostYield() { waits.fetch_add(1,std::memory_order_relaxed); }
void Expect(bool condition,const char* why) {
    if(!condition) { std::fprintf(stderr,"Demon's Souls idle thunk: %s\n",why); std::abort(); }
}
}

int main() {
    static_assert(UseIdleBackoff(true, true, nullptr));
    static_assert(!UseIdleBackoff(true, false, nullptr));
    static_assert(!UseIdleBackoff(false, true, nullptr));
    static_assert(!UseIdleBackoff(false, false, nullptr));
    static_assert(!UseIdleBackoff(true, true, "0"));
    static_assert(UseIdleBackoff(true, true, "1"));
    static_assert(UseIdleBackoff(false, false, "1"));
    Expect(Matches(CallBytes,PollBytes),"verified Bluepoint byte signatures");
    Expect(IdleBackoffMicroseconds(1) == 50u, "short BPE empty poll retains low latency");
    Expect(IdleBackoffMicroseconds(63) == 50u, "short poll threshold");
    Expect(IdleBackoffMicroseconds(64) == 200u, "sustained polls slow down");
    Expect(IdleBackoffMicroseconds(512) == 800u, "long sustained polls park briefly");
    Expect(HasGeometryBranch(GeometryBranchBytes), "verified Bluepoint original index branch");
    auto wrong_geom = GeometryBranchBytes;
    wrong_geom[GeometryJumpOpcodeOffset] = 0xeb;
    Expect(!HasGeometryBranch(wrong_geom), "already patched geometry cannot patch twice");
    wrong_geom = GeometryBranchBytes;
    wrong_geom[18] ^= 0x01;
    Expect(!HasGeometryBranch(wrong_geom), "different game binary rejected");

    auto bad=CallBytes;
    bad[20]^=1;
    Expect(!Matches(bad,PollBytes),"reject changed call after branch");
    Expect(!Matches(CallBytes,std::span(PollBytes).first(5)),"reject shortened poll");
    std::array<uint8_t,Thunk.size()> code{};
    std::array<uint8_t,5> call{};
    Expect(MakeThunk(0x10000000,0x10000200,0x12345678,code),"guest patch offsets");
    Expect(MakeCall(0x10000500,0x10000000,call),"guest relative call offsets");
    int32_t rel=0;
    std::memcpy(&rel,code.data()+PollRel32Offset,4);
    Expect(0x10000000u+9+rel==0x10000200u,"guest poll call displacement");
    uint64_t target=0;
    std::memcpy(&target,code.data()+HleTrapImm64Offset,8);
    Expect(target==0x12345678,"host HLE trap pointer");
    Expect(code.back()==0xc3 && call.front()==0xe8,"x86 return and call opcode");
    Expect(!MakeThunk(0x10000000,0xf00000000ULL,0x1234,code),"reject out-of-range relative poll");
    Expect(!MakeThunk(0x10000000,0x10000200,0,code),"reject missing HLE trap");

#if defined(__x86_64__) && !defined(_WIN32)
    // Synthetic guest executable code, not user game bytes:
    // [cave x86 thunk][original poll][call site]. A host function stands in
    // for the ARM64 FEX guest-visible HLE trap, so the same x86 thunk executes.
    auto* page=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                              MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    Expect(page!=MAP_FAILED,"test executable cave allocation");
    const auto cave=reinterpret_cast<uint64_t>(page);
    const auto poll=cave+0x200;
    const auto site=cave+0x300;
    Expect(MakeThunk(cave,poll,reinterpret_cast<uint64_t>(&HostYield),code),"live guest poll displacement");
    Expect(MakeCall(site,cave,call),"live guest call site displacement");
    std::memcpy(page,code.data(),code.size());
    std::memcpy(page+0x300,call.data(),call.size());
    page[0x305]=0xc3; // the call site returns to the native test harness
    using Invoke=int(*)();
    // Call the thunk with the same ABI stack alignment as the game's
    // patched CALL instruction. A synthetic CALL at function entry would
    // add an extra stack frame and falsely misalign FXSAVE64.
    auto invoke=reinterpret_cast<Invoke>(page);
    page[0x200]=0x31; page[0x201]=0xc0; page[0x202]=0xc3; // xor eax,eax; ret
    waits.store(0);
    Expect(invoke()==0 && waits.load()==1,"no-job poll yields once, guest zero return preserved");
    page[0x200]=0xb8; page[0x201]=1; page[0x202]=0; page[0x203]=0;
    page[0x204]=0; page[0x205]=0xc3; // mov eax,1; ret
    __builtin___clear_cache(reinterpret_cast<char*>(page),reinterpret_cast<char*>(page+0x306));
    Expect(invoke()==1 && waits.load()==1,"work available: no yielding; original return preserved");
    munmap(page,4096);
#endif
    std::puts("Demon's Souls CPU-poll tests: verified signatures, guest x86 thunk, host HLE bridge semantics");
    return 0;
}
