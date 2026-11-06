#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Libs::Graphics::RealtimeClock {
inline uint32_t PeriodPs(bool extension, bool feature, float timestamp_period_ns) {
    const double ps = static_cast<double>(timestamp_period_ns) * 1000.0;
    if (!extension || !feature || !std::isfinite(ps) || ps < 0.5 ||
        ps > static_cast<double>(std::numeric_limits<uint32_t>::max()) - 0.5) return 0;
    return static_cast<uint32_t>(std::llround(ps));
}
// PC reference 77a8f307: shift fast device clocks toward the guest's 100 MHz
// timebase. Slow clocks (including Adreno's counter) remain monotonic unshifted.
inline uint32_t Shift(uint32_t period_ps) {
    return period_ps ? std::bit_width(7500u / period_ps) : 0u;
}
}
