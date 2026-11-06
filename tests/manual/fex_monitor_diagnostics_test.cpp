#include "cpu/fexDecodeDiagnostics.h"
#include "cpu/guestThreadProbe.h"
#include "cpu/monitorWaitXProbe.h"
#include <cassert>
#include <algorithm>
#include <cstring>
#include <thread>
#include <sys/mman.h>
int main() {
    using namespace Cpu::DecodeDiagnostics;
    uint64_t address = 0;
    assert(AddressFromMessage("Invalid instruction in entry block: 900041127", address));
    assert(address == 0x900041127ull);
    assert(!AddressFromMessage("other FEX message", address));
    assert(!AddressFromMessage("Invalid instruction in entry block: x", address));
    const std::array<uint8_t, 4> source{0x0f, 0x01, 0xfa, 0xc3};
    std::array<uint8_t, 4> copied{};
    assert(ReadSelf(reinterpret_cast<uint64_t>(source.data()), copied.data(), copied.size()) == copied.size());
    assert(source == copied);
    assert(ReadSelf(1, copied.data(), copied.size()) == 0);
    // A real frame-chain layout, a cycle, an unreadable successor, and invalid
    // stack bounds must all terminate without faulting or scanning raw stacks.
    alignas(16) std::array<uint64_t, 6> frames{};
    const auto frame_base = reinterpret_cast<uint64_t>(frames.data());
    frames = {frame_base + 16, 0x90083b583, frame_base + 32, 0x90083c1e0, 0, 0x900841645};
    std::array<uint64_t, 24> callers{};
    assert(Cpu::ThreadProbe::WalkFrames(frame_base, frame_base, callers) == 3);
    assert(callers[0] == 0x90083b583 && callers[2] == 0x900841645);
    frames[2] = frame_base; // backwards/cyclic chain
    assert(Cpu::ThreadProbe::WalkFrames(frame_base, frame_base, callers) == 2);
    frames[0] = frame_base + 16;
    frames[2] = frame_base + 1024 * 1024 + 32; // outside bounded step
    assert(Cpu::ThreadProbe::WalkFrames(frame_base, frame_base, callers) == 2);
    assert(Cpu::ThreadProbe::WalkFrames(1, 8, callers) == 0);
    assert(Cpu::ThreadProbe::WalkFrames(frame_base + 8, frame_base, callers) == 0);
    Cpu::ThreadProbe::Snapshot sample;
    sample.rip = 0x900041127;
    sample.exact = true;
    sample.gpr.fill(sample.rip);
    Cpu::ThreadProbe::Publish(0, sample);
    Cpu::ThreadProbe::Snapshot read;
    assert(Cpu::ThreadProbe::Read(0, read));
    assert(read.rip == sample.rip && read.exact && read.gpr == sample.gpr);
    std::atomic<bool> running{true};
    std::thread writer([&] {
        for (uint64_t i = 1; i < 10000; ++i) {
            sample.rip = i;
            sample.gpr.fill(i);
            Cpu::ThreadProbe::Publish(0, sample);
        }
        running.store(false);
    });
    while (running.load()) {
        if (Cpu::ThreadProbe::Read(0, read))
            for (auto reg : read.gpr) assert(reg == read.rip);
    }
    writer.join();
#if defined(__x86_64__)
    // Test the probe's own branch offsets and failure detection on this host.
    // MONITORX/MWAITX are replaced with known no-op/PAUSE only in the harness;
    // the FEX test and Android startup check execute the real instruction bytes.
    auto code = Cpu::GuestCpu::MonitorWaitXProbeCode;
    constexpr std::array<uint8_t, 3> monitor{0x0f,0x01,0xfa};
    constexpr std::array<uint8_t, 3> wait{0x0f,0x01,0xfb};
    const auto mi = std::search(code.begin(), code.end(), monitor.begin(), monitor.end());
    const auto wi = std::search(code.begin(), code.end(), wait.begin(), wait.end());
    assert(mi != code.end() && wi != code.end());
    const size_t wait_offset = wi - code.begin();
    std::fill_n(mi, 3, 0x90);
    const uint8_t pause[]{0xf3,0x90,0x90};
    std::copy_n(pause, 3, wi);
    auto* page = static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(page != MAP_FAILED);
    std::memcpy(page, code.data(), code.size());
    auto invoke = reinterpret_cast<uint64_t(*)()>(page);
    assert(invoke() == 1);
    const uint8_t corrupt[]{0x83,0xca,0x01}; // or edx,1 must fail register/flag check
    std::memcpy(page+wait_offset, corrupt, 3);
    assert(invoke() == 0);
    munmap(page,4096);
#endif
    std::puts("FEX diagnostics: safe decode capture, coherent signal mailbox, probe success/failure paths passed");
}
