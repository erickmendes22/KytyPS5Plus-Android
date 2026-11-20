#ifndef KYTY_RENDERER_UFC_PERF_H_
#define KYTY_RENDERER_UFC_PERF_H_

#include <chrono>
#include <cstdint>

namespace Libs::Graphics::UfcPerf {
// Low-overhead, UFC6-only telemetry. A single atomic pair per timed site,
// with one session.log report per second. No sampling signals, GPU readbacks,
// extra Vulkan submissions, or threads. GPU hardware busy is NOT inferred.
enum class Site : uint8_t {
    Pm4, GpuQueueBlocked, VkQueueSubmit, SubmitDrain, TimelineWait,
    Finish, DescriptorPrepare, DescriptorUpdate, GarbageCollect, Count
};
[[nodiscard]] bool Enabled();
void Add(Site site, uint64_t duration_ns);
void NoteDescriptor(bool reused);
void NoteSubmit(uint32_t work_items);
void NoteFan(bool enabled);
void ReportMaybe();

class Scoped {
public:
    explicit Scoped(Site site): m_site(site) {
        if (Enabled()) m_start = std::chrono::steady_clock::now();
    }
    ~Scoped() {
        if (m_start != std::chrono::steady_clock::time_point{}) {
            const auto elapsed = std::chrono::steady_clock::now() - m_start;
            Add(m_site, static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
        }
    }
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;
private:
    Site m_site;
    std::chrono::steady_clock::time_point m_start{};
};
} // namespace Libs::Graphics::UfcPerf

#endif
