#include "loader/demonsSoulsIdle.h"
#include "loader/runtimeLinker.h"
#include "loader/guestHostFunction.h"
#include "loader/systemContent.h"
#include "common/virtualMemory.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/memory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace Loader::DemonsSoulsIdle {
namespace {
uint64_t g_cave=0,g_site=0,g_geometry_site=0;
std::array<uint8_t,5> g_installed{};
std::atomic_uint64_t g_idle_sleeps{0};

// The trap is an x86 guest function on ARM64 thanks to GuestHost::AddressOf;
// FEX's HLE thunk executes this native sleep on the worker's actual host thread.
// Do NOT use a native ARM64 function address in the x86 stub!
static void KYTY_SYSV_ABI NativeIdleBackoff() {
    // A worker only enters this native HLE callback after the verified guest
    // poll returned "no work". Short-lived empty queues retain the original
    // 50-us delay; sustained busy polling progressively sleeps longer without
    // changing the guest poll result, registers or job submission semantics.
    thread_local uint32_t empty_streak = 0;
    thread_local auto last_sleep = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (last_sleep == std::chrono::steady_clock::time_point {} ||
        now - last_sleep > std::chrono::milliseconds(2)) {
        empty_streak = 0;
    }
    if (empty_streak < 4096u) ++empty_streak;
    const auto delay_us = IdleBackoffMicroseconds(empty_streak);
    std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    last_sleep = std::chrono::steady_clock::now();
    const auto total = g_idle_sleeps.fetch_add(1,std::memory_order_relaxed) + 1;
    if (total == 1 || (total % 1000000) == 0) {
        std::printf("[DS_IDLE] empty_job_yields=%llu backoff_us=%u\n",
                    static_cast<unsigned long long>(total),delay_us);
    }
}
void Note(const std::string& message) {
    Log::WriteToConsoleAndLog(std::string("[DS_IDLE] ")+message+"\n");
}
bool Enabled() {
#if defined(__ANDROID__) && defined(__aarch64__)
    return UseIdleBackoff(true, true, std::getenv("KYTY_DS_IDLE_BACKOFF"));
#else
    return UseIdleBackoff(false, false, std::getenv("KYTY_DS_IDLE_BACKOFF"));
#endif
}
bool IsSupportedGame() {
    std::string title;
    // APP_VER can be written as 01.07, 01.007 or 01.007.000; do not use
    // its textual spelling as an executable identity. Install checks both
    // actual EBOOT instruction signatures before changing anything.
    return SystemContentParamSfoGetString("TITLE_ID", &title) && title=="PPSA01341";
}
void InstallGeometryBranch(Program* program) {
    if (g_geometry_site || program->mapped_size < GeometryBranchOffset + GeometryBranchBytes.size() ||
        program->base_vaddr > UINT64_MAX - GeometryBranchOffset - GeometryBranchBytes.size()) {
        return;
    }
    const uint64_t site = program->base_vaddr + GeometryBranchOffset;
    if (!Libs::Graphics::HostMemoryRangeIsReadable(site, GeometryBranchBytes.size())) {
        Note("geometry branch is not readable; leaving original guest code");
        return;
    }
    const std::span<const uint8_t> code(
        reinterpret_cast<const uint8_t*>(site), GeometryBranchBytes.size());
    if (!HasGeometryBranch(code)) {
        Note("geometry branch signature differs; leaving original guest code");
        return;
    }
    // JNE rel8 -> JMP rel8. Same size and same branch target. All other
    // instructions, including the guest's normal index path, are untouched.
    // This is runtime-only and reversible; eboot.bin is NEVER modified.
    const uint64_t opcode = site + GeometryJumpOpcodeOffset;
    const uint8_t jmp = 0xeb;
    std::memcpy(reinterpret_cast<void*>(opcode), &jmp, sizeof(jmp));
    if (!Common::VirtualMemory::FlushInstructionCache(opcode, sizeof(jmp))) {
        const uint8_t old = GeometryBranchBytes[GeometryJumpOpcodeOffset];
        std::memcpy(reinterpret_cast<void*>(opcode), &old, sizeof(old));
        (void)Common::VirtualMemory::FlushInstructionCache(opcode, sizeof(old));
        Note("geometry instruction-cache flush failed; original code restored");
        return;
    }
    g_geometry_site = site;
    Note("verified BPE guest GPU-culling bypass installed; original CPU indices retained");
}
} // namespace

void Install(Program* program) {
    if (!program || program->file_name.filename()!="eboot.bin" || !IsSupportedGame()) {
        return;
    }
    // Geometry must not depend on whether the optional idle backoff is enabled.
    InstallGeometryBranch(program);
    if (!Enabled()) return;
    if (g_cave || program->mapped_size < PollOffset+PollBytes.size() ||
        program->mapped_size >= CaveOffset ||
        program->base_vaddr > UINT64_MAX-CaveOffset-CaveSize) {
        Note("disabled: no executable space or already installed");
        return;
    }
    const uint64_t site=program->base_vaddr+CallOffset;
    const uint64_t poll=program->base_vaddr+PollOffset;
    if (!Libs::Graphics::HostMemoryRangeIsReadable(site,CallBytes.size()) ||
        !Libs::Graphics::HostMemoryRangeIsReadable(poll,PollBytes.size())) {
        Note("disabled: executable signatures unreadable");
        return;
    }
    const auto call=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(site),CallBytes.size());
    const auto pollCode=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(poll),PollBytes.size());
    if (!Matches(call,pollCode)) {
        Note("disabled: EBOOT call/poll signatures do not match verified version");
        return;
    }
    const uint64_t requested=program->base_vaddr+CaveOffset;
    const uint64_t cave=Libs::LibKernel::Memory::AllocateRuntimeMemory(
        requested,CaveSize,Common::VirtualMemory::Mode::ExecuteReadWrite,
        "demons_souls_idle_poll_arm64",true);
    if (cave!=requested) {
        if (cave) Libs::LibKernel::Memory::FreeGuestMemory(cave,CaveSize);
        Note("disabled: guest code cave not available");
        return;
    }
    const uint64_t guestTrap=GuestHost::AddressOf<&NativeIdleBackoff>("Demon's Souls idle worker host backoff");
    std::array<uint8_t,Thunk.size()> code{};
    std::array<uint8_t,5> patched{};
    if (!MakeThunk(cave,poll,guestTrap,code) || !MakeCall(site,cave,patched)) {
        Libs::LibKernel::Memory::FreeGuestMemory(cave,CaveSize);
        Note("disabled: guest code range/displacement invalid");
        return;
    }
    std::memcpy(reinterpret_cast<void*>(cave),code.data(),code.size());
    if (!Common::VirtualMemory::FlushInstructionCache(cave,code.size()) ||
        !Libs::LibKernel::Memory::ProtectGuestMemory(
            cave,CaveSize,Common::VirtualMemory::Mode::ExecuteRead,nullptr)) {
        Libs::LibKernel::Memory::FreeGuestMemory(cave,CaveSize);
        Note("disabled: guest thunk cannot be made executable");
        return;
    }
    // Only after all preflights, write the CALL in the validated guest EBOOT.
    std::memcpy(reinterpret_cast<void*>(site),patched.data(),patched.size());
    if (!Common::VirtualMemory::FlushInstructionCache(site,patched.size())) {
        std::memcpy(reinterpret_cast<void*>(site),CallBytes.data(),patched.size());
        Common::VirtualMemory::FlushInstructionCache(site,patched.size());
        Libs::LibKernel::Memory::FreeGuestMemory(cave,CaveSize);
        Note("disabled: guest call-site instruction cache flush failed");
        return;
    }
    g_cave=cave;
    g_site=site;
    g_installed=patched;
    Note("installed: verified Bluepoint BPE poll, original poll preserved; adaptive FEX HLE empty-job backoff 50-800 us");
}

void Clear() {
    if (g_geometry_site != 0) {
        const uint64_t opcode = g_geometry_site + GeometryJumpOpcodeOffset;
        const uint8_t patched = 0xeb;
        if (Libs::Graphics::HostMemoryRangeIsReadable(opcode, 1) &&
            *reinterpret_cast<const uint8_t*>(opcode) == patched) {
            const uint8_t old = GeometryBranchBytes[GeometryJumpOpcodeOffset];
            std::memcpy(reinterpret_cast<void*>(opcode), &old, sizeof(old));
            (void)Common::VirtualMemory::FlushInstructionCache(opcode, sizeof(old));
        }
        g_geometry_site = 0;
    }
    if(!g_cave) return;
    const auto active=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(g_site),g_installed.size());
    if(std::ranges::equal(active,g_installed)) {
        std::memcpy(reinterpret_cast<void*>(g_site),CallBytes.data(),g_installed.size());
        Common::VirtualMemory::FlushInstructionCache(g_site,g_installed.size());
    }
    Libs::LibKernel::Memory::FreeGuestMemory(g_cave,CaveSize);
    Note("uninstalled; total adaptive empty-job sleeps="+std::to_string(g_idle_sleeps.exchange(0)));
    g_cave=g_site=0;
}
} // namespace Loader::DemonsSoulsIdle
