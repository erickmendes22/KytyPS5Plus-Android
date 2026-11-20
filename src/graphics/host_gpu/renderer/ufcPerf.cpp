#include "graphics/host_gpu/renderer/ufcPerf.h"
#include "common/logging/log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics::UfcPerf {
namespace {
constexpr size_t N = static_cast<size_t>(Site::Count);
std::array<std::atomic<uint64_t>, N> g_count{};
std::array<std::atomic<uint64_t>, N> g_ns{};
std::atomic<uint64_t> g_reused{0}, g_written{0}, g_submits{0}, g_work{0};
std::atomic<uint64_t> g_fan_on{0}, g_fan_off{0};
std::atomic<int64_t> g_last{0};
int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("KYTY_UFC6_PERF");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}
void Add(Site site, uint64_t ns) {
    if (!Enabled()) return;
    const auto i = static_cast<size_t>(site);
    g_count[i].fetch_add(1, std::memory_order_relaxed);
    g_ns[i].fetch_add(ns, std::memory_order_relaxed);
}
void NoteDescriptor(bool reused) {
    if (!Enabled()) return;
    (reused ? g_reused : g_written).fetch_add(1, std::memory_order_relaxed);
}
void NoteSubmit(uint32_t work) {
    if (!Enabled()) return;
    g_submits.fetch_add(1, std::memory_order_relaxed);
    g_work.fetch_add(work, std::memory_order_relaxed);
}
void NoteFan(bool on) {
    if (!Enabled()) return;
    (on ? g_fan_on : g_fan_off).fetch_add(1, std::memory_order_relaxed);
}
void ReportMaybe() {
    if (!Enabled()) return;
    const auto now = NowNs();
    auto old = g_last.load(std::memory_order_relaxed);
    if (old == 0) {
        (void)g_last.compare_exchange_strong(old, now, std::memory_order_relaxed);
        return;
    }
    if (now - old < 1'000'000'000ll ||
        !g_last.compare_exchange_strong(old, now, std::memory_order_relaxed)) return;
    const double seconds = static_cast<double>(now - old) * 1e-9;
    std::array<uint64_t, N> counts{}, total{};
    for (size_t i = 0; i < N; i++) {
        counts[i] = g_count[i].exchange(0, std::memory_order_relaxed);
        total[i] = g_ns[i].exchange(0, std::memory_order_relaxed);
    }
    const auto ms = [&](Site s) { return total[static_cast<size_t>(s)] * 1e-6 / seconds; };
    const auto calls = [&](Site s) { return counts[static_cast<size_t>(s)] / seconds; };
    const auto reuse = g_reused.exchange(0, std::memory_order_relaxed);
    const auto write = g_written.exchange(0, std::memory_order_relaxed);
    const auto submits = g_submits.exchange(0, std::memory_order_relaxed);
    const auto work = g_work.exchange(0, std::memory_order_relaxed);
    const auto fanon = g_fan_on.exchange(0, std::memory_order_relaxed);
    const auto fanoff = g_fan_off.exchange(0, std::memory_order_relaxed);
    Log::WriteToConsoleAndLog(fmt::format(
        "[UFC6_PERF] dt={:.2f}s pm4_cpu={:.1f}ms/s blocked={:.1f}ms/s "
        "submit_driver={:.1f}ms/s({:.1f}/s) submit_drain={:.1f}ms/s "
        "timeline_wait={:.1f}ms/s finish={:.1f}ms/s descriptor_prepare={:.1f}ms/s "
        "descriptor_update={:.1f}ms/s gc={:.1f}ms/s sets_reused={} sets_written={} "
        "submits={} work={} fan_on={} fan_off={} gpu_busy=not_measured\n",
        seconds, ms(Site::Pm4), ms(Site::GpuQueueBlocked),
        ms(Site::VkQueueSubmit), calls(Site::VkQueueSubmit), ms(Site::SubmitDrain),
        ms(Site::TimelineWait), ms(Site::Finish), ms(Site::DescriptorPrepare),
        ms(Site::DescriptorUpdate), ms(Site::GarbageCollect),
        reuse, write, submits, work, fanon, fanoff));
}
} // namespace Libs::Graphics::UfcPerf
