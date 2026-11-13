#include "loader/demonsSoulsIdle.h"
#include "loader/demonsSoulsHdMobile.h"
#include "loader/runtimeLinker.h"
#include "loader/guestHostFunction.h"
#include "loader/systemContent.h"
#include "common/virtualMemory.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>
#include <string_view>
#include <limits>
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

struct HdPatch {
    uint64_t address;
    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
};
std::vector<HdPatch> g_hd_patches;

// Do not assume a US/JP eboot or a different update has identical bytes.
// Search only within 4 KiB of independently documented BPE 01.007.000 sites,
// then require exactly one match. No match -> no modification to guest code.
std::optional<uint64_t> FindHdSignature(Program* program, uint64_t hint,
                                        std::string_view signature) {
    using namespace DemonsSoulsHdMobile;
    constexpr uint64_t Radius = 4096;
    const size_t count = SignatureBytes(signature);
    if (count == 0 || program->mapped_size < count ||
        program->base_vaddr > UINT64_MAX - program->mapped_size) return {};
    const uint64_t first = hint > Radius ? hint - Radius : 0u;
    const uint64_t last = std::min<uint64_t>(program->mapped_size, hint + Radius + count);
    if (last < first + count) return {};
    const uint64_t address = program->base_vaddr + first;
    const auto length = last - first;
    if (!Libs::Graphics::HostMemoryRangeIsReadable(address, length)) return {};
    const auto data = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(address), static_cast<size_t>(length));
    std::optional<uint64_t> result;
    uint64_t best_distance = UINT64_MAX;
    for (size_t at = 0; at + count <= data.size(); ++at) {
        if (data[at] != static_cast<uint8_t>((HexDigit(signature[0]) << 4u) |
                                             HexDigit(signature[1]))) continue;
        if (!DemonsSoulsHdMobile::Matches(data.subspan(at, count), signature)) continue;
        const uint64_t offset = first + at;
        const uint64_t distance = offset > hint ? offset - hint : hint - offset;
        if (distance == best_distance) return {}; // equally plausible sites
        if (distance < best_distance) {
            // Two legitimate 3840x2160 display-buffer calls lie only ~240
            // bytes apart: resolve each by its independently verified RVA.
            best_distance = distance;
            result = address + at;
        }
    }
    return result;
}
void UndoHdOutput() {
    for (auto it = g_hd_patches.rbegin(); it != g_hd_patches.rend(); ++it) {
        if (!Libs::Graphics::HostMemoryRangeIsReadable(it->address, it->after.size()))
            continue;
        // Preserve patches applied by the game or other modules after us.
        if (std::memcmp(reinterpret_cast<const void*>(it->address),
                        it->after.data(), it->after.size()) != 0) continue;
        std::memcpy(reinterpret_cast<void*>(it->address),
                    it->before.data(), it->before.size());
        (void)Common::VirtualMemory::FlushInstructionCache(it->address, it->before.size());
    }
    g_hd_patches.clear();
}
bool WriteHd(uint64_t address, const void* bytes, size_t count) {
    if (count == 0 || !Libs::Graphics::HostMemoryRangeIsReadable(address, count))
        return false;
    HdPatch edit {address, std::vector<uint8_t>(count),
                  std::vector<uint8_t>(count)};
    std::memcpy(edit.before.data(), reinterpret_cast<const void*>(address), count);
    std::memcpy(edit.after.data(), bytes, count);
    g_hd_patches.push_back(std::move(edit));
    std::memcpy(reinterpret_cast<void*>(address), bytes, count);
    return Common::VirtualMemory::FlushInstructionCache(address, count);
}
void InstallHdOutput(Program* program) {
#if defined(__ANDROID__) && defined(__aarch64__)
    // The hardware 1080p layout completed installation, but the 2026-11-12
    // on-device test still showed the game world black and the HUD oversized.
    // Restore the last known default (guest output mode 3) for normal launches.
    // Only the explicit opt-in should activate the signature-validated mode 1.
    const char* choice = std::getenv("KYTY_DS_HD_GAME_OUTPUT");
    if (!choice || choice[0] != '1' || choice[1] != '\0') return;
    if (!g_hd_patches.empty()) return;
    using namespace DemonsSoulsHdMobile;
    const auto video = FindHdSignature(program, 0x9fe6a3, VideoChoice);
    const auto renderer = FindHdSignature(program, 0xb26055, RendererChoice);
    const auto ui = FindHdSignature(program, 0xde88b6, InterfaceChoice);
    const auto buffer_a = FindHdSignature(program, 0x90af74, DisplayBuffer);
    const auto buffer_b = FindHdSignature(program, 0x90b061, DisplayBuffer);
    const auto setter = FindHdSignature(program, 0x90a6a7, SetterFixedPair);
    const auto builder = FindHdSignature(program, 0x90b210, ConstructorFixedPair);
    const auto query = FindHdSignature(program, 0x90ac3c, QueryFixedPair);
    if (!video || !renderer || !ui || !buffer_a || !buffer_b ||
        !setter || !builder || !query || *buffer_a == *buffer_b) {
        Note("HD video mode: one or more 01.007.000 signatures absent; original 4K output retained");
        return;
    }
    constexpr std::array<uint8_t, 3> nop3 {0x0f, 0x1f, 0x00};
    constexpr std::array<uint8_t, 4> nop4 {0x0f, 0x1f, 0x40, 0x00};
    const auto write_mode = [&](uint64_t address) {
        return WriteHd(address + 1, &VideoMode, sizeof(VideoMode));
    };
    const auto set_buffer = [&](uint64_t address) {
        return WriteHd(address + 1, &OutputWidth, sizeof(OutputWidth)) &&
               WriteHd(address + 6, &OutputHeight, sizeof(OutputHeight));
    };
    // Atomic at the operation level: all signatures were validated before
    // any patch, and a failed code-cache flush restores every changed site.
    const bool ok = write_mode(*video) && write_mode(*renderer) && write_mode(*ui) &&
        WriteHd(*video + 7, nop3.data(), nop3.size()) &&
        WriteHd(*video + 12, nop3.data(), nop3.size()) &&
        WriteHd(*renderer + 27, nop3.data(), nop3.size()) &&
        WriteHd(*renderer + 32, nop3.data(), nop3.size()) &&
        WriteHd(*ui + 7, nop3.data(), nop3.size()) &&
        WriteHd(*ui + 16, nop4.data(), nop4.size()) &&
        set_buffer(*buffer_a) && set_buffer(*buffer_b) &&
        WriteHd(*setter + 6, &PackedCeiling, sizeof(PackedCeiling)) &&
        WriteHd(*builder + 9, &PackedCeiling, sizeof(PackedCeiling)) &&
        WriteHd(*query + 5, &PackedCeiling, sizeof(PackedCeiling));
    if (!ok) {
        UndoHdOutput();
        Note("HD video mode: patch failed; original guest instructions restored");
        return;
    }
    // Mode 1 (1600x900 3D -> 1920x1080) is intentional: the PC fork
    // reproduced missing 3D character models with mode 2 at 1080 output.
    // 720p Android window is scaled from the game's native HD video mode.
    Note("HD video mode: validated 01.007.000; output=1920x1080 "
         "renderer=mode1(1600x900) UI=HD (reversible; fixes PC menu-mode mismatch)");
#else
    (void)program;
#endif
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
    // The working Chen KytyPS5 keeps Bluepoint's native GPU index-culling
    // path. An earlier Android experiment forced JMP at EBOOT+0xA0D6FB,
    // bypassing that path on *every* launch, at the same time the launcher
    // disabled doIndexBufferCulling. That changed the producer of the indices
    // used by every 3D draw before Vulkan ever saw them. Do not replace a
    // conditional guest branch without explicit opt-in.
    const char* cpu_indices = std::getenv("KYTY_DS_FORCE_CPU_INDICES");
    if (cpu_indices != nullptr && cpu_indices[0] == '1' && cpu_indices[1] == '\0') {
        InstallGeometryBranch(program);
    }
    InstallHdOutput(program);
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
    UndoHdOutput();
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
