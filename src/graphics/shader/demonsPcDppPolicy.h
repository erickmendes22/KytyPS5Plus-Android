#pragma once
#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics::DemonsPcDppPolicy {
inline constexpr uint64_t RayProducer = 0x684e797dce67bb8eull;
inline constexpr uint64_t Relight = 0xbcbcc6ba1e68c974ull;
inline constexpr uint64_t Projector = 0x567d21ad45386c8dull;
constexpr bool Applies(bool enabled, uint64_t hash) {
    return enabled && (hash == RayProducer || hash == Relight || hash == Projector);
}
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("KYTY_DS_PC_DPP");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
constexpr bool CaptureProducer(bool trace, uint64_t hash) {
    return trace && hash == RayProducer;
}
// Capture the actual producer before the later consumer has chosen probes.
// The 769 run performed only ten active dispatches; an adaptive HDR gate missed all
// of them in the current session. Keep a small independent cap, never capture idle dispatches.
constexpr bool CaptureActive(uint32_t valid_count, bool late_due) {
    return (valid_count != 0 && valid_count <= 16) || late_due;
}
} // namespace Libs::Graphics::DemonsPcDppPolicy
