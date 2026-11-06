#pragma once
#include <cstdlib>
#include <string_view>

namespace Common::GuestTrace {
// Observation only. The legacy Demon trace variable remains accepted without
// enabling any of that title's clock, idle-backoff or frame-order policies.
inline bool Enabled() {
    static const bool enabled = [] {
        const auto on = [](const char* name) {
            const char* value = std::getenv(name);
            return value && std::string_view(value) == "1";
        };
        return on("KYTY_GUEST_FRAME_TRACE") || on("KYTY_DS_FRAME_TRACE");
    }();
    return enabled;
}
}
