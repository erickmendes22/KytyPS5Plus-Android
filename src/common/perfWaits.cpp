#include "common/perfWaits.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Common::PerfStats {

namespace Detail {

std::atomic<uint64_t> g_wait_ns[static_cast<size_t>(Wait::Count)] {};
std::atomic<uint64_t> g_wait_count[static_cast<size_t>(Wait::Count)] {};
std::atomic<uint64_t> g_counters[static_cast<size_t>(Counter::Count)] {};

int64_t MonotonicNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

} // namespace Detail

namespace {

thread_local Wait     t_wait_site  = Wait::SemOther;
thread_local bool     t_gpu_thread = false;
thread_local uint32_t t_pm4_opcode = 0;

// Readbacks of the current period, by (site, reader, 4 KiB page).
struct ReadbackStats {
	uint64_t count   = 0;
	uint64_t bytes   = 0;
	uint64_t wait_ns = 0;
	uint64_t lag_sum = 0;
	uint64_t lag_max = 0;
	std::array<uint64_t, static_cast<size_t>(ReadbackMode::Count)> modes {};
};
using ReadbackKey = std::tuple<uint8_t, uint32_t, uint64_t>;
std::mutex                            g_readback_mutex;
std::map<ReadbackKey, ReadbackStats>  g_readbacks;

constexpr std::array<const char*, static_cast<size_t>(ReadbackMode::Count)> kReadbackModeNames = {
    "copy", "done", "wait", "flush"};

constexpr std::array<const char*, static_cast<size_t>(Wait::Count)> kWaitNames = {
    "idle",       "blocked",  "flip_done",   "flip_slot", "sync_cmd", "fault",
    "submit",     "process",  "prio_wait", "tl_query", "finish",   "flush",       "stream",    "mesh_ring", "download",
    "read_fault", "write_fault", "indirect_args", "metadata", "gc",   "fault_buf",
    "swapchain",  "desc",     "prio",        "other",     "fault",    "sync_cmd",
    "suspend",    "flip_done", "wait_idle",
};

static_assert(kWaitNames.back() != nullptr, "every Wait needs a report name");

constexpr size_t Index(Wait kind) {
	return static_cast<size_t>(kind);
}

uint64_t TakeCounter(Counter kind) {
	return Detail::g_counters[static_cast<size_t>(kind)].exchange(0, std::memory_order_relaxed);
}

void AppendWaits(std::string& out, const char* group, Wait first, Wait last, double per_frame) {
	out += ' ';
	out += group;
	out += ':';
	for (auto i = Index(first); i <= Index(last); i++) {
		const auto ns    = Detail::g_wait_ns[i].exchange(0, std::memory_order_relaxed);
		const auto count = Detail::g_wait_count[i].exchange(0, std::memory_order_relaxed);
		if (count == 0) {
			continue;
		}
		char part[80];
		std::snprintf(part, sizeof(part), " %s=%.1f(%.0f)", kWaitNames[i],
		              static_cast<double>(ns) / 1e6 * per_frame,
		              static_cast<double>(count) * per_frame);
		out += part;
	}
}

} // namespace

void Detail::LogLongWait(Wait kind, int64_t start_ns, uint64_t ns) {
	// Idle and background waits are not stalls.
	if (kind == Wait::GpuIdle || kind == Wait::SemPriority || kind == Wait::GpuProcess) {
		return;
	}
	char line[128];
	std::snprintf(line, sizeof(line), "[perf-event] t=%.1f wait %s%s %.1fms",
	              static_cast<double>(start_ns) / 1e6,
	              Index(kind) >= Index(Wait::GuestFault) ? "guest:" : "", kWaitNames[Index(kind)],
	              static_cast<double>(ns) / 1e6);
	EmitLine(line);
}

ScopedWaitSite::ScopedWaitSite(Wait site): previous_(t_wait_site) {
	t_wait_site = site;
}

ScopedWaitSite::~ScopedWaitSite() {
	t_wait_site = previous_;
}

void EmitLine(const std::string& line) {
	// Also kept in a file: logcat drops lines under load.
	static std::mutex file_mutex;
	static FILE*      file = std::fopen("_perf_log.txt", "w");
	if (file != nullptr) {
		std::lock_guard lock(file_mutex);
		std::fprintf(file, "%s\n", line.c_str());
		std::fflush(file);
	}
#if defined(__ANDROID__)
	__android_log_write(ANDROID_LOG_INFO, "KytyPerf", line.c_str());
#else
	std::fprintf(stderr, "%s\n", line.c_str());
#endif
}

void SetGpuThread() {
	t_gpu_thread = true;
}

bool IsGpuThread() {
	return t_gpu_thread;
}

Wait CurrentWaitSite() {
	return t_wait_site;
}

void SetPm4Opcode(uint32_t opcode) {
	t_pm4_opcode = opcode;
}

uint32_t CurrentPm4Opcode() {
	return t_pm4_opcode;
}

void RecordReadback(Wait site, uint32_t reader, uint64_t address, uint64_t bytes, uint64_t wait_ns,
                    uint64_t lag, ReadbackMode mode) {
	if (!Enabled() || mode >= ReadbackMode::Count) {
		return;
	}
	std::lock_guard lock(g_readback_mutex);
	auto& stats = g_readbacks[{static_cast<uint8_t>(site), reader, address >> 12u}];
	stats.count++;
	stats.bytes += bytes;
	stats.wait_ns += wait_ns;
	stats.lag_sum += lag;
	stats.lag_max = std::max(stats.lag_max, lag);
	stats.modes[static_cast<size_t>(mode)]++;
}

std::string TakeReadbackSummary(double seconds, uint64_t flips) {
	std::map<ReadbackKey, ReadbackStats> taken;
	{
		std::lock_guard lock(g_readback_mutex);
		taken.swap(g_readbacks);
	}
	const double per_frame = flips == 0 ? 1.0 / seconds : 1.0 / static_cast<double>(flips);
	const auto   faults    = [&](Counter kind) {
        return static_cast<double>(TakeCounter(kind)) * per_frame;
	};
	char head[384];
	// Eviction totals for the whole period (not per frame), to line up with its fps and p95.
	const auto evictions = TakeCounter(Counter::GcEvictions);
	const auto evicted   = TakeCounter(Counter::GcEvictedBytes);
	const auto downloads = TakeCounter(Counter::GcDownloads);
	std::snprintf(head, sizeof(head),
	              "[perf-readback] per %s: faults gpu r=%.1f w=%.1f guest r=%.1f w=%.1f "
	              "idle_submits=%.1f indirect gpu=%.1f cpu=%.1f (empty=%.1f) "
	              "gc_evict=%llu (%.1f MiB, %llu downloaded) |",
	              flips == 0 ? "second" : "frame", faults(Counter::FaultsGpuRead),
	              faults(Counter::FaultsGpuWrite), faults(Counter::FaultsGuestRead),
	              faults(Counter::FaultsGuestWrite), faults(Counter::IdleSubmits),
	              faults(Counter::IndirectDrawsGpu), faults(Counter::IndirectDrawsCpu),
	              faults(Counter::IndirectDrawsCpuEmpty),
	              static_cast<unsigned long long>(evictions),
	              static_cast<double>(evicted) / (1024.0 * 1024.0),
	              static_cast<unsigned long long>(downloads));
	std::string line = head;
	if (taken.empty()) {
		return line + " none";
	}
	std::array<uint64_t, static_cast<size_t>(ReadbackMode::Count)> modes {};
	uint64_t count = 0, wait_ns = 0;
	std::vector<std::pair<ReadbackKey, ReadbackStats>> sorted(taken.begin(), taken.end());
	for (const auto& [key, stats]: sorted) {
		count += stats.count;
		wait_ns += stats.wait_ns;
		for (size_t i = 0; i < modes.size(); i++) {
			modes[i] += stats.modes[i];
		}
	}
	char part[256];
	std::snprintf(part, sizeof(part), " n=%.1f wait=%.1fms", static_cast<double>(count) * per_frame,
	              static_cast<double>(wait_ns) / 1e6 * per_frame);
	line += part;
	for (size_t i = 0; i < modes.size(); i++) {
		std::snprintf(part, sizeof(part), " %s=%.1f", kReadbackModeNames[i],
		              static_cast<double>(modes[i]) * per_frame);
		line += part;
	}
	line += " | top:";
	// Heaviest readers first (by wait time, then count); a handful is enough to name them.
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
		return std::tie(a.second.wait_ns, a.second.count) > std::tie(b.second.wait_ns, b.second.count);
	});
	constexpr size_t kTop = 6;
	for (size_t i = 0; i < std::min(kTop, sorted.size()); i++) {
		const auto& [key, stats] = sorted[i];
		const auto [site, reader, page] = key;
		char reader_name[16];
		if (reader == kReaderGuestRead) {
			std::snprintf(reader_name, sizeof(reader_name), "guest-r");
		} else if (reader == kReaderGuestWrite) {
			std::snprintf(reader_name, sizeof(reader_name), "guest-w");
		} else if (reader == kReaderOther) {
			std::snprintf(reader_name, sizeof(reader_name), "other");
		} else {
			std::snprintf(reader_name, sizeof(reader_name), "pm4:%03x", reader);
		}
		std::snprintf(part, sizeof(part),
		              " [%s %s 0x%010llx n=%llu %lluB wait=%.1fms lag=%.1f/%llu %s=%llu]",
		              reader_name, kWaitNames[site], static_cast<unsigned long long>(page << 12u),
		              static_cast<unsigned long long>(stats.count),
		              static_cast<unsigned long long>(stats.bytes),
		              static_cast<double>(stats.wait_ns) / 1e6,
		              static_cast<double>(stats.lag_sum) / static_cast<double>(stats.count),
		              static_cast<unsigned long long>(stats.lag_max),
		              kReadbackModeNames[static_cast<size_t>(std::max_element(stats.modes.begin(), stats.modes.end()) - stats.modes.begin())],
		              static_cast<unsigned long long>(*std::max_element(stats.modes.begin(), stats.modes.end())));
		line += part;
	}
	return line;
}

std::string TakeWaitSummary(double seconds, uint64_t flips) {
	// Per presented frame; with no flip in the period, per second.
	const double per_frame = flips == 0 ? 1.0 / seconds : 1.0 / static_cast<double>(flips);
	const auto   gpu_ns    = TakeCounter(Counter::GpuBusyNs);
	const auto   timed     = TakeCounter(Counter::GpuSubmitsTimed);
	char         head[1024];
	std::snprintf(
	    head, sizeof(head),
	    "[perf-wait] per %s: gpu_busy=%.1fms (%.0f%% of wall, %.0f cmdbufs) submits=%.0f "
	    "passes=%.0f draws=%.0f dispatches=%.0f mesh=%.0f (%.0fK idx) up=%.0fKB img_up=%.0fKB "
	    "down=%.0fKB (%.0f sync) img_down=%.0fKB video: req=%.0f got=%.0f starved=%.0f "
	    "up=%.0f black=%.0f races=%.0f vo_refresh=%.0f blank=%.0f pass_end: state=%.0f "
	    "transit=%.0f attach=%.0f (same=%.0f) image=%.0f buffer=%.0f compute=%.0f write=%.0f submit=%.0f "
	    "other=%.0f hoisted=%.0f rescale: up=%.0f down=%.0f | "
	    "ms(count):",
	    flips == 0 ? "second" : "frame", static_cast<double>(gpu_ns) / 1e6 * per_frame,
	    static_cast<double>(gpu_ns) / 1e7 / seconds, static_cast<double>(timed) * per_frame,
	    static_cast<double>(TakeCounter(Counter::Submits)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::RenderPasses)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::Draws)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::Dispatches)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::MeshDispatches)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::MeshIndices)) / 1e3 * per_frame,
	    static_cast<double>(TakeCounter(Counter::UploadBytes)) / 1024.0 * per_frame,
	    static_cast<double>(TakeCounter(Counter::ImageUploadBytes)) / 1024.0 * per_frame,
	    static_cast<double>(TakeCounter(Counter::DownloadBytes)) / 1024.0 * per_frame,
	    static_cast<double>(TakeCounter(Counter::SyncDownloads)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::ImageDownloadBytes)) / 1024.0 * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoFrameRequests)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoFrameDelivered)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoFrameStarved)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoUploads)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoBlackUploads)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoUploadRaces)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::VideoOutRefreshUploads)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::BlankPresents)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndState)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndTransit)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndAttach)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndAttachSame)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndImageCopy)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndBuffer)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndCompute)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndShaderWrite)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndSubmit)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::PassEndOther)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::HoistedBarriers)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::RenderScaleUpBlits)) * per_frame,
	    static_cast<double>(TakeCounter(Counter::RenderScaleDownBlits)) * per_frame);
	std::string line = head;
	AppendWaits(line, "gpu-thread", Wait::GpuIdle, Wait::TimelineQuery, per_frame);
	AppendWaits(line, "sem", Wait::SemFinish, Wait::SemOther, per_frame);
	AppendWaits(line, "guest", Wait::GuestFault, Wait::GuestIdle, per_frame);
	return line;
}

} // namespace Common::PerfStats
