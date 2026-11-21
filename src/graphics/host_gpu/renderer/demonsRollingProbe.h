#pragma once
#include <atomic>
#include <cstdint>

namespace Libs::Graphics::DemonsRollingProbe {

// A bounded, ongoing sample gate. No absolute guest-frame milestones:
// captures follow the game for short sessions as well as long gameplay.
// The first matching event always records; later events are spaced by
// *relative* progress, never a predetermined "last" frame. A restarted
// game counter can begin a new series without restarting the emulator.
// A single atomic CAS owns each capture, even across recording threads.
class FrameGate {
public:
    [[nodiscard]] bool Due(int32_t frame, int32_t spacing) noexcept {
        if (frame < 0 || spacing < 1) return false;
        int32_t previous = m_last.load(std::memory_order_relaxed);
        for (;;) {
            if (previous >= 0 && frame >= previous &&
                int64_t{frame} - int64_t{previous} < spacing) return false;
            // Ignore out-of-order feedback from an older frame, except a
            // restarted guest counter that begins again from frame zero.
            if (previous >= 0 && frame < previous && frame > 4) return false;
            if (m_last.compare_exchange_weak(previous, frame, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) return true;
        }
    }
private:
    std::atomic<int32_t> m_last{-1};
};

} // namespace Libs::Graphics::DemonsRollingProbe
