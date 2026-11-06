#pragma once
#include <cstdint>
#include <cstdlib>
#include <string_view>
namespace Cpu::GuestTscPolicy {
// Set only for PPSA01341 before FEX initializes. Its BPE scheduler uses raw
// RDTSC together with sceKernelGetTscFrequency, which exposes CNTFRQ_EL0.
inline bool NativeCounterRequested() {
    static const bool native = [] {
        const char* value = std::getenv("KYTY_DS_NATIVE_TSC");
        return value && std::string_view(value) == "1";
    }();
    return native;
}
inline constexpr uint8_t Scale(uint64_t frequency, bool native_counter) {
    uint8_t scale = 0;
    if (native_counter) return 0;
    while (frequency != 0 && frequency < 1'000'000'000) {
        frequency <<= 1u;
        ++scale;
    }
    return scale;
}
} // namespace Cpu::GuestTscPolicy
