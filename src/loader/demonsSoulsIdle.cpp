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
uint64_t g_cave=0,g_site=0;
std::array<uint8_t,5> g_installed{};
std::atomic_uint64_t g_idle_sleeps{0};

// The trap is an x86 guest function on ARM64 thanks to GuestHost::AddressOf;
// FEX's HLE thunk executes this native sleep on the worker's actual host thread.
// Do NOT use a native ARM64 function address in the x86 stub!
static void KYTY_SYSV_ABI NativeIdleBackoff() {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    const auto total = g_idle_sleeps.fetch_add(1,std::memory_order_relaxed) + 1;
    if (total == 1 || (total % 100000) == 0) {
        // Log only at the first successful ARM64 HLE callback, then sparsely.
        // Unlike LOGF, stdout is captured even when printf_direction=Silent.
        std::printf("[DS_IDLE] host_hle_yields=%llu (empty Bluepoint jobs slept 50 us)\n",
                    static_cast<unsigned long long>(total));
    }
}
void Note(const char* message) {
    Log::WriteToConsoleAndLog(std::string("[DS_IDLE] ")+message+"\n");
}
bool Enabled() {
    const char* env=std::getenv("KYTY_DS_IDLE_BACKOFF");
    return env && env[0]=='1' && env[1]=='\0';
}
bool IsSupportedGame() {
    std::string title,version;
    const bool hasTitle=SystemContentParamSfoGetString("TITLE_ID",&title);
    const bool hasVersion=SystemContentParamSfoGetString("APP_VER",&version);
    if (!hasTitle || !hasVersion || title!="PPSA01341" ||
        (version!="01.007.000" && version!="01.007")) return false;
    return true;
}
} // namespace

void Install(Program* program) {
    if (!Enabled()) return;
    if (!program || program->file_name.filename()!="eboot.bin" || !IsSupportedGame()) {
        Note("disabled: executable or title/version not supported");
        return;
    }
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
    Note("installed: verified Bluepoint BPE poll, original poll is preserved; FEX HLE 50-us yield on empty job only");
}

void Clear() {
    if(!g_cave) return;
    const auto active=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(g_site),g_installed.size());
    if(std::ranges::equal(active,g_installed)) {
        std::memcpy(reinterpret_cast<void*>(g_site),CallBytes.data(),g_installed.size());
        Common::VirtualMemory::FlushInstructionCache(g_site,g_installed.size());
    }
    Libs::LibKernel::Memory::FreeGuestMemory(g_cave,CaveSize);
    Note("uninstalled; total 50-us empty-job sleeps="+std::to_string(g_idle_sleeps.exchange(0)));
    g_cave=g_site=0;
}
} // namespace Loader::DemonsSoulsIdle
