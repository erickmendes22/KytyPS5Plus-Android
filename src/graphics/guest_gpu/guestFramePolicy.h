#pragma once
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

// The host PC fork defines the guest frame boundary at submission admission,
// not at the moment the graphics processor happens to process a suspend packet.
// All access is serialized by GuestGpu's producer/queue mutexes.
class GuestFramePolicy {
public:
    uint64_t Admit() { return m_next_sequence++; }
    bool ConsumeGraphicsReset() { return std::exchange(m_reset_next_graphics, false); }

    // Return the PREVIOUS boundary to wait for (two guest frames on the CP).
    // A subsequent graphics/flip submission owns the context reset.
    uint64_t EndFrame() {
        m_reset_next_graphics = true;
        const auto wait_target = m_last_boundary;
        m_last_boundary = m_next_sequence - 1;
        return wait_target;
    }
private:
    uint64_t m_next_sequence = 1;
    uint64_t m_last_boundary = 0;
    bool m_reset_next_graphics = false;
};

} // namespace Libs::Graphics
