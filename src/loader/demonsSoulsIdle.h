#pragma once
// Title- and byte-signature-verified idle Bluepoint worker poll adapter.
// The guest x86 thunk calls the original poll, and only on "no work" calls
// an ARM64-safe guest HLE trap to yield the host worker for 50 us.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace Loader { struct Program; namespace DemonsSoulsIdle {
void Install(Program* program);
void Clear();

// Pure policy kept separate from the FEX code cave to permit synthetic tests.
// A version string is not executable identity: signatures gate actual patching.
inline constexpr bool UseIdleBackoff(bool android, bool arm64, const char* env) {
    if (env != nullptr && env[0]=='0' && env[1]=='\0') return false;
    if (env != nullptr && env[0]=='1' && env[1]=='\0') return true;
    return android && arm64;
}
// The verified Bluepoint empty-job path can run millions of times while
// the GPU is almost idle. Keep short polls responsive, but avoid waking 13
// BPE workers at 20 kHz apiece after sustained inactivity.
inline constexpr uint32_t IdleBackoffMicroseconds(uint32_t consecutive_empty) {
    return consecutive_empty < 64 ? 50u : consecutive_empty < 512 ? 200u : 800u;
}
static_assert(IdleBackoffMicroseconds(1) == 50);
static_assert(IdleBackoffMicroseconds(64) == 200);
static_assert(IdleBackoffMicroseconds(512) == 800);

// Executable-verified guest branch observed in the supplied PPSA01341 eboot.
// At image+0xA0D6FB it tests doIndexBufferCulling (BPE CVar+0xCAA):
//   cmp byte ptr [rcx+0xcaa], 1; jne +0x20
// The JNE bypasses GPU-produced indices; replacing just JNE by JMP
// preserves the original guest index buffer even if PackageCmdLineArgs.txt
// is ignored on a particular boot. Never patch an unverified executable.
inline constexpr uint64_t GeometryBranchOffset = 0xA0D6FB;
inline constexpr size_t GeometryJumpOpcodeOffset = 11;
inline constexpr std::array<uint8_t,45> GeometryBranchBytes{
    0x48,0x8b,0x49,0x68,0x80,0xb9,0xaa,0x0c,0x00,0x00,0x01,0x75,0x20,
    0x41,0xc6,0x86,0xec,0xda,0x02,0x00,0x01,0x48,0xc7,0x03,0x00,0x00,
    0x00,0x00,0xc5,0xf8,0x10,0x80,0xf0,0x0d,0x00,0x00,0xc4,0xc1,0x78,
    0x11,0x86,0xb8,0xda,0x02,0x00
};
inline bool HasGeometryBranch(std::span<const uint8_t> guest) {
    return std::ranges::equal(guest, GeometryBranchBytes);
}
static_assert(GeometryBranchBytes[GeometryJumpOpcodeOffset] == 0x75);
static_assert(GeometryBranchBytes[GeometryJumpOpcodeOffset + 1] == 0x20);

inline constexpr uint64_t CallOffset=0x83b59b, PollOffset=0x83b880;
inline constexpr uint64_t CaveOffset=0x8000000, CaveSize=0x4000;
inline constexpr std::array<uint8_t,24> CallBytes{
  0xe8,0xe0,0x02,0x00,0x00,0x84,0xc0,0x74,0xd7,0x4c,0x8b,0x7d,
  0xc8,0x49,0x8b,0xb7,0x90,0x00,0x00,0x00,0x48,0x85,0xf6,0x74};
inline constexpr std::array<uint8_t,24> PollBytes{
  0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
  0x53,0x48,0x81,0xec,0x88,0x00,0x00,0x00,0x4c,0x8b,0x35,0x25};
inline bool Matches(std::span<const uint8_t> call,std::span<const uint8_t> poll) {
 return std::ranges::equal(call,CallBytes)&&std::ranges::equal(poll,PollBytes);
}
// Assembled with GNU as x86-64, precisely the PC poll/thunk semantics:
// sub rsp,8; call poll; add rsp,8; test al,al; jnz done; pushfq;
// preserve all caller-saved integer GPRs and FXSAVE64/FXRSTOR64;
// movabs rax, guest-visible HLE trap address; call rax;
// restore state; ret.  Never place an ARM64 native function pointer in guest code.
inline constexpr std::array<uint8_t,82> Thunk{
  0x48,0x83,0xec,0x08,0xe8,0x00,0x00,0x00,0x00,0x48,0x83,0xc4,
  0x08,0x84,0xc0,0x75,0x40,0x9c,0x50,0x51,0x52,0x56,0x57,0x41,
  0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x48,0x81,0xec,0x08,0x02,
  0x00,0x00,0x48,0x0f,0xae,0x04,0x24,0x48,0xb8,0x88,0x77,0x66,
  0x55,0x44,0x33,0x22,0x11,0xff,0xd0,0x48,0x0f,0xae,0x0c,0x24,
  0x48,0x81,0xc4,0x08,0x02,0x00,0x00,0x41,0x5b,0x41,0x5a,0x41,
  0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,0x9d,0xc3
};
inline constexpr size_t PollRel32Offset=5, HleTrapImm64Offset=45;
inline bool MakeThunk(uint64_t cave,uint64_t poll,uint64_t guest_trap,
                      std::array<uint8_t,82>& output) {
 if(cave>UINT64_MAX-Thunk.size()||poll>INT64_MAX||cave>INT64_MAX-Thunk.size()||
    guest_trap==0) return false;
 const auto relative=static_cast<int64_t>(poll)-static_cast<int64_t>(cave+9);
 if(relative<INT32_MIN||relative>INT32_MAX) return false;
 output=Thunk;
 const auto displacement=static_cast<int32_t>(relative);
 std::memcpy(output.data()+PollRel32Offset,&displacement,4);
 std::memcpy(output.data()+HleTrapImm64Offset,&guest_trap,8);
 return true;
}
inline bool MakeCall(uint64_t site,uint64_t cave,std::array<uint8_t,5>& result) {
 if(site>INT64_MAX-5||cave>INT64_MAX) return false;
 const auto disp=static_cast<int64_t>(cave)-static_cast<int64_t>(site+5);
 if(disp<INT32_MIN||disp>INT32_MAX) return false;
 result[0]=0xe8;
 const auto d=static_cast<int32_t>(disp);
 std::memcpy(result.data()+1,&d,4);
 return true;
}
} } // namespace Loader::DemonsSoulsIdle
