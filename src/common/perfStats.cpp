#include "common/perfStats.h"

#include "common/perfWaits.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace Common::PerfStats::Detail {

std::atomic<bool>     g_enabled {false};
std::atomic<uint64_t> g_hle_calls {0};
std::atomic<uint64_t> g_gpu_faults {0};
std::atomic<uint64_t> g_flips {0};
std::atomic<uint64_t> g_audio_underruns {0};
std::atomic<uint32_t> g_hle_by_id[kMaxHleIds] {};

} // namespace Common::PerfStats::Detail

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace Common::PerfStats {
namespace {

std::atomic<int64_t>  g_input_mark_ns {0};
std::atomic<HleAddressResolver> g_hle_resolver {nullptr};
constexpr size_t      kStallKinds = static_cast<size_t>(StallKind::Count);
std::atomic<uint64_t> g_stall_count[kStallKinds] {};
std::atomic<uint64_t> g_stall_us[kStallKinds] {};
std::atomic<uint64_t> g_flip_latency_sum_us {0};
std::atomic<uint64_t> g_flip_latency_max_us {0};
std::atomic<uint64_t> g_present_sum_us {0};
std::atomic<uint64_t> g_present_max_us {0};
std::atomic<int64_t>  g_last_flip_ns {0};
std::atomic<uint64_t> g_flip_gap_max_us {0};
constexpr uint64_t    kLongEventUs = 5000; // stalls logged individually with a timestamp
constexpr size_t      kTranslatePhases = static_cast<size_t>(TranslatePhase::Count);
std::atomic<uint64_t> g_translate_phase_us[kTranslatePhases] {};
std::atomic<uint64_t> g_skipped_draws {0};
// Per-thread phase totals: a long stall event prints the phases that ran inside it.
thread_local uint64_t t_translate_phase_us[kTranslatePhases] {};

const char* StallName(StallKind kind) {
	switch (kind) {
		case StallKind::PipelineCompile: return "pipeline_compile";
		case StallKind::ShaderTranslate: return "shader_translate";
		case StallKind::ShaderWorker: return "shader_worker";
		case StallKind::PipelineWorker: return "pipeline_worker";
		default: return "stall";
	}
}
std::mutex            g_frame_mutex;
std::vector<uint32_t> g_frame_times_us; // flip intervals of the current period

// Every flip interval also goes to _perf_frames.txt ("monotonic_ms interval_ms") for
// whole-run percentiles and hitch timelines (long waits and stalls are logged on the same clock).
void RecordFrameTime(int64_t now_ns, uint64_t gap_us) {
	static FILE*    file = std::fopen("_perf_frames.txt", "w");
	std::lock_guard      lock(g_frame_mutex);
	g_frame_times_us.push_back(static_cast<uint32_t>(std::min<uint64_t>(gap_us, UINT32_MAX)));
	if (file != nullptr) {
		std::fprintf(file, "%.1f %.2f\n", static_cast<double>(now_ns) / 1e6,
		             static_cast<double>(gap_us) / 1000.0);
		std::fflush(file);
	}
}

void StoreMax(std::atomic<uint64_t>& target, uint64_t value) {
	uint64_t current = target.load(std::memory_order_relaxed);
	while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
	}
}
std::atomic<uint64_t> g_input_queue_ns {0};

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

} // namespace

void CountFlip(uint64_t submit_to_present_us, uint64_t present_call_us) {
	if (!Enabled()) {
		return;
	}
	Detail::g_flips.fetch_add(1, std::memory_order_relaxed);
	g_flip_latency_sum_us.fetch_add(submit_to_present_us, std::memory_order_relaxed);
	StoreMax(g_flip_latency_max_us, submit_to_present_us);
	g_present_sum_us.fetch_add(present_call_us, std::memory_order_relaxed);
	StoreMax(g_present_max_us, present_call_us);
	const int64_t now  = NowNs();
	const int64_t last = g_last_flip_ns.exchange(now, std::memory_order_relaxed);
	if (last != 0) {
		const auto gap_us = static_cast<uint64_t>(now - last) / 1000;
		StoreMax(g_flip_gap_max_us, gap_us);
		RecordFrameTime(now, gap_us);
	}
}

// Frame pacing over the last period, as text for the report line; resets the period.
std::string TakeFramePercentiles() {
	std::vector<uint32_t> times;
	{
		std::lock_guard lock(g_frame_mutex);
		times.swap(g_frame_times_us);
	}
	if (times.empty()) {
		return "frame_ms p50=- p95=- max=-";
	}
	std::sort(times.begin(), times.end());
	const auto at = [&times](double q) {
		return static_cast<double>(times[static_cast<size_t>(q * static_cast<double>(times.size() - 1))]) / 1000.0;
	};
	char buf[96];
	std::snprintf(buf, sizeof(buf), "frame_ms p50=%.1f p95=%.1f max=%.1f", at(0.5), at(0.95),
	              static_cast<double>(times.back()) / 1000.0);
	return buf;
}

std::string TakeFrameSummary(uint64_t flips) {
	const uint64_t latency_sum = g_flip_latency_sum_us.exchange(0, std::memory_order_relaxed);
	const uint64_t latency_max = g_flip_latency_max_us.exchange(0, std::memory_order_relaxed);
	const uint64_t present_sum = g_present_sum_us.exchange(0, std::memory_order_relaxed);
	const uint64_t present_max = g_present_max_us.exchange(0, std::memory_order_relaxed);
	const uint64_t gap_max     = g_flip_gap_max_us.exchange(0, std::memory_order_relaxed);
	const double   n           = flips == 0 ? 1.0 : static_cast<double>(flips);
	const auto take = [](StallKind kind, uint64_t* micros) {
		*micros = g_stall_us[static_cast<size_t>(kind)].exchange(0, std::memory_order_relaxed);
		return g_stall_count[static_cast<size_t>(kind)].exchange(0, std::memory_order_relaxed);
	};
	uint64_t       pipeline_us = 0;
	uint64_t       shader_us   = 0;
	uint64_t       worker_us   = 0;
	const uint64_t pipelines   = take(StallKind::PipelineCompile, &pipeline_us);
	const uint64_t shaders     = take(StallKind::ShaderTranslate, &shader_us);
	const uint64_t workers     = take(StallKind::ShaderWorker, &worker_us);
	uint64_t       pipeline_worker_us = 0;
	const uint64_t pipeline_workers   = take(StallKind::PipelineWorker, &pipeline_worker_us);
	double         phase_ms[kTranslatePhases] {};
	for (size_t i = 0; i < kTranslatePhases; i++) {
		phase_ms[i] = static_cast<double>(g_translate_phase_us[i].exchange(0, std::memory_order_relaxed)) / 1000.0;
	}
	const uint64_t skipped = g_skipped_draws.exchange(0, std::memory_order_relaxed);
	char           buf[512];
	std::snprintf(buf, sizeof(buf), "shaders=%llu (%.1fms) pipelines=%llu (%.1fms) flip_gap_max=%.1fms submit->present avg=%.1f max=%.1fms present_call avg=%.1f max=%.1fms"
	              " | shader_workers=%llu (%.1fms) pipeline_workers=%llu (%.1fms) skipped_draws=%llu xlate ir=%.0f plan=%.0f mat=%.0f emit=%.0f mod=%.0f ms",
	              static_cast<unsigned long long>(shaders), static_cast<double>(shader_us) / 1000.0,
	              static_cast<unsigned long long>(pipelines), static_cast<double>(pipeline_us) / 1000.0,
	              static_cast<double>(gap_max) / 1000.0, static_cast<double>(latency_sum) / n / 1000.0,
	              static_cast<double>(latency_max) / 1000.0, static_cast<double>(present_sum) / n / 1000.0,
	              static_cast<double>(present_max) / 1000.0, static_cast<unsigned long long>(workers),
	              static_cast<double>(worker_us) / 1000.0, static_cast<unsigned long long>(pipeline_workers),
	              static_cast<double>(pipeline_worker_us) / 1000.0, static_cast<unsigned long long>(skipped),
	              phase_ms[0], phase_ms[1], phase_ms[2], phase_ms[3], phase_ms[4]);
	return buf;
}

void CountStall(StallKind kind, uint64_t micros) {
	if (Enabled() && kind < StallKind::Count) {
		if (micros >= kLongEventUs) {
			char line[96];
			std::snprintf(line, sizeof(line), "[perf-event] t=%.1f %s %.1fms",
			              static_cast<double>(NowNs()) / 1e6, StallName(kind),
			              static_cast<double>(micros) / 1000.0);
			EmitLine(line);
		}
		g_stall_count[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
		g_stall_us[static_cast<size_t>(kind)].fetch_add(micros, std::memory_order_relaxed);
	}
}

ScopedStall::ScopedStall(StallKind kind, const char* label)
    : kind_(kind), label_(label), start_ns_(Enabled() ? NowNs() : 0) {
	if (start_ns_ != 0 && (kind == StallKind::ShaderTranslate || kind == StallKind::ShaderWorker)) {
		std::fill(std::begin(t_translate_phase_us), std::end(t_translate_phase_us), uint64_t {0});
	}
}

ScopedStall::~ScopedStall() {
	if (start_ns_ == 0) {
		return;
	}
	const auto micros = static_cast<uint64_t>(NowNs() - start_ns_) / 1000;
	if ((kind_ == StallKind::ShaderTranslate || kind_ == StallKind::ShaderWorker) && micros >= kLongEventUs) {
		char line[192];
		std::snprintf(line, sizeof(line),
		              "[perf-event] t=%.1f %s %s phases ir=%.1f plan=%.1f mat=%.1f emit=%.1f mod=%.1f ms",
		              static_cast<double>(NowNs()) / 1e6, StallName(kind_),
		              label_ != nullptr ? label_ : "-",
		              static_cast<double>(t_translate_phase_us[0]) / 1000.0,
		              static_cast<double>(t_translate_phase_us[1]) / 1000.0,
		              static_cast<double>(t_translate_phase_us[2]) / 1000.0,
		              static_cast<double>(t_translate_phase_us[3]) / 1000.0,
		              static_cast<double>(t_translate_phase_us[4]) / 1000.0);
		EmitLine(line);
	}
	CountStall(kind_, micros);
}

void AddTranslatePhase(TranslatePhase phase, uint64_t micros) {
	if (Enabled() && phase < TranslatePhase::Count) {
		t_translate_phase_us[static_cast<size_t>(phase)] += micros;
		g_translate_phase_us[static_cast<size_t>(phase)].fetch_add(micros, std::memory_order_relaxed);
	}
}

ScopedTranslatePhase::ScopedTranslatePhase(TranslatePhase phase)
    : phase_(phase), start_ns_(Enabled() ? NowNs() : 0) {}

ScopedTranslatePhase::~ScopedTranslatePhase() {
	if (start_ns_ != 0) {
		AddTranslatePhase(phase_, static_cast<uint64_t>(NowNs() - start_ns_) / 1000);
	}
}

void CountSkippedDraw() {
	if (Enabled()) {
		g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
	}
}

void SetHleAddressResolver(HleAddressResolver resolver) {
	g_hle_resolver.store(resolver, std::memory_order_release);
}

HleAddressResolver GetHleAddressResolver() {
	return g_hle_resolver.load(std::memory_order_acquire);
}

void MarkInput(uint64_t queue_delay_ns) {
	if (!Enabled()) {
		return;
	}
	g_input_queue_ns.store(queue_delay_ns, std::memory_order_relaxed);
	g_input_mark_ns.store(NowNs(), std::memory_order_release);
}

void ObserveInput() {
	if (!Enabled() || g_input_mark_ns.load(std::memory_order_relaxed) == 0) {
		return;
	}
	const int64_t mark = g_input_mark_ns.exchange(0, std::memory_order_acq_rel);
	if (mark == 0) {
		return;
	}
	std::fprintf(stderr, "[perf] input: SDL queue %.1f ms, event loop -> guest pad read %.1f ms\n",
	             static_cast<double>(g_input_queue_ns.load(std::memory_order_relaxed)) / 1e6,
	             static_cast<double>(NowNs() - mark) / 1e6);
}

} // namespace Common::PerfStats

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Common::PerfStats {
namespace {

using Detail::g_enabled;

constexpr int      kSampleSignal       = SIGPROF;
constexpr long     kSampleIntervalNs   = 1'000'000; // 1 ms of thread CPU time per sample
constexpr size_t   kMaxFrames          = 10;
constexpr size_t   kRingSize           = size_t {1} << 16;
constexpr uint64_t kMaxFrameSpan       = 1u << 20; // a caller frame lies within 1 MiB above the callee's
constexpr int      kProfileDumpSeconds = 5;
constexpr size_t   kTopEntries         = 80;
constexpr int      kWallTag            = 0x57a11;
constexpr size_t   kWallStackFrames    = 8;

struct Sample {
	std::atomic<uint64_t> seq {0};
	int32_t               tid = 0;
	uint32_t              depth = 0;
	uint32_t              weight = 1; // expirations covered: CPU-time timers only fire on scheduler ticks
	bool                  wall   = false; // from a wall-clock timer (SampleThisThreadWallClock)
	uint64_t              frames[kMaxFrames] {};
};

std::array<Sample, kRingSize> g_ring {};
std::atomic<uint64_t>         g_write {0};

// Reads 16 bytes of the caller's frame record without risking a fault: the frame pointer of JIT or
// foreign code may be garbage, and a fault inside this handler would be fatal.
bool ReadFrameRecord(uint64_t fp, uint64_t* out) {
	iovec local {out, 2 * sizeof(uint64_t)};
	iovec remote {reinterpret_cast<void*>(fp), 2 * sizeof(uint64_t)};
	return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == 2 * sizeof(uint64_t);
}

void OnSample(int /*sig*/, siginfo_t* info, void* context) {
	const int saved_errno = errno;
	auto*     uc          = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
	const uint64_t pc = uc->uc_mcontext.pc;
	const uint64_t lr = uc->uc_mcontext.regs[30];
	uint64_t       fp = uc->uc_mcontext.regs[29];
	const uint64_t sp = uc->uc_mcontext.sp;
#elif defined(__x86_64__)
	const uint64_t pc = uc->uc_mcontext.gregs[REG_RIP];
	const uint64_t lr = 0;
	uint64_t       fp = uc->uc_mcontext.gregs[REG_RBP];
	const uint64_t sp = uc->uc_mcontext.gregs[REG_RSP];
#else
	const uint64_t pc = 0, lr = 0, sp = 0;
	uint64_t       fp = 0;
#endif
	const uint64_t index = g_write.fetch_add(1, std::memory_order_relaxed);
	Sample&        s     = g_ring[index & (kRingSize - 1)];
	s.tid                = static_cast<int32_t>(syscall(SYS_gettid));
	s.weight             = 1 + static_cast<uint32_t>(info->si_overrun > 0 ? info->si_overrun : 0);
	s.wall               = info->si_code == SI_TIMER && info->si_value.sival_int == kWallTag;
	s.frames[0]          = pc;
	s.frames[1]          = lr;
	uint32_t depth       = 2;
	uint64_t low         = sp;
	while (depth < kMaxFrames && fp >= low && fp - low < kMaxFrameSpan && (fp & 7) == 0) {
		uint64_t record[2] {};
		if (!ReadFrameRecord(fp, record) || record[1] == 0) {
			break;
		}
		s.frames[depth++] = record[1];
		low               = fp + 16;
		fp                = record[0];
	}
	s.depth = depth;
	s.seq.store(index + 1, std::memory_order_release);
	errno = saved_errno;
}

struct Mapping {
	uint64_t    start  = 0;
	uint64_t    end    = 0;
	uint64_t    offset = 0;
	std::string name;
};

std::vector<Mapping> ReadMappings() {
	std::vector<Mapping> maps;
	std::ifstream        in("/proc/self/maps");
	std::string          line;
	while (std::getline(in, line)) {
		unsigned long long start = 0, end = 0, offset = 0;
		char               perms[8] {};
		int                name_pos = 0;
		if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &start, &end, perms, &offset, &name_pos) < 4) {
			continue;
		}
		if (perms[2] != 'x') {
			continue;
		}
		std::string name = name_pos > 0 ? line.substr(static_cast<size_t>(name_pos)) : std::string {};
		if (name.empty()) {
			name = "[anon-exec]";
		} else if (auto slash = name.rfind('/'); slash != std::string::npos && name[0] == '/') {
			name = name.substr(slash + 1);
		}
		maps.push_back({start, end, offset, name});
	}
	return maps;
}

class Symbolizer {
public:
	// Allows one re-read of /proc/self/maps per reporting period for addresses not seen before.
	void NewPeriod() {
		refreshed_ = false;
	}

	// "module+0xFILEOFFSET", or the module name alone for anonymous code.
	std::string Locate(uint64_t pc) {
		const Mapping* m = Lookup(pc);
		if (m == nullptr) {
			return "[unknown]";
		}
		if (m->name[0] == '[') {
			return m->name;
		}
		char buf[32];
		std::snprintf(buf, sizeof(buf), "+0x%llx", static_cast<unsigned long long>(pc - m->start + m->offset));
		return m->name + buf;
	}

	const std::string& Module(uint64_t pc) {
		static const std::string unknown = "[unknown]";
		const Mapping*           m       = Lookup(pc);
		return m == nullptr ? unknown : m->name;
	}

private:
	const Mapping* Lookup(uint64_t pc) {
		const Mapping* m = Find(pc);
		if (m == nullptr && !refreshed_) {
			refreshed_ = true;
			Load();
			m = Find(pc);
		}
		return m;
	}

	void Load() {
		maps_ = ReadMappings();
		std::sort(maps_.begin(), maps_.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
	}

	const Mapping* Find(uint64_t pc) const {
		auto it = std::upper_bound(maps_.begin(), maps_.end(), pc,
		                           [](uint64_t value, const Mapping& m) { return value < m.start; });
		if (it == maps_.begin()) {
			return nullptr;
		}
		--it;
		return pc < it->end ? &*it : nullptr;
	}

	std::vector<Mapping> maps_;
	bool                 refreshed_ = false;
};

std::string ThreadName(int tid) {
	std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/comm");
	std::string   name;
	std::getline(in, name);
	return name;
}

std::set<int> ListThreads() {
	std::set<int> tids;
	if (DIR* dir = opendir("/proc/self/task"); dir != nullptr) {
		while (const dirent* e = readdir(dir)) {
			if (e->d_name[0] != '.') {
				tids.insert(std::atoi(e->d_name));
			}
		}
		closedir(dir);
	}
	return tids;
}

// One CPU-time timer per thread, delivering the sample signal to that thread only.
class ThreadTimers {
public:
	void Sync() {
		const auto live = ListThreads();
		for (auto it = timers_.begin(); it != timers_.end();) {
			if (live.count(it->first) == 0) {
				syscall(SYS_timer_delete, it->second);
				it = timers_.erase(it);
			} else {
				++it;
			}
		}
		for (int tid: live) {
			if (timers_.count(tid) == 0) {
				Arm(tid);
			}
		}
	}

private:
	void Arm(int tid) {
		// MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)
		const auto clock = static_cast<clockid_t>((~static_cast<unsigned>(tid) << 3) | 6);
		sigevent   sev {};
		sev.sigev_notify          = SIGEV_THREAD_ID;
		sev.sigev_signo           = kSampleSignal;
		sev._sigev_un._tid        = tid;
		int timer                 = -1;
		if (syscall(SYS_timer_create, clock, &sev, &timer) != 0) {
			timers_[tid] = -1;
			return;
		}
		itimerspec spec {};
		spec.it_interval.tv_nsec = kSampleIntervalNs;
		spec.it_value.tv_nsec    = kSampleIntervalNs;
		syscall(SYS_timer_settime, timer, 0, &spec, nullptr);
		timers_[tid] = timer;
	}

	std::map<int, int> timers_;
};

void Emit(const std::string& line) {
	EmitLine(line);
}

const char* Category(const std::string& module) {
	if (module == "[anon-exec]") {
		return "jit";
	}
	if (module.find("libmain") != std::string::npos || module.find("kyty") != std::string::npos) {
		return "kyty";
	}
	if (module.find("vulkan") != std::string::npos || module.find("adreno") != std::string::npos ||
	    module.find("libgsl") != std::string::npos || module.find("glnext") != std::string::npos) {
		return "gpu-driver";
	}
	if (module == "libc.so" || module == "libc++.so" || module == "libc++_shared.so" || module == "libm.so") {
		return "libc";
	}
	if (module.find("SDL") != std::string::npos) {
		return "sdl";
	}
	if (module.find("[vdso]") != std::string::npos) {
		return "vdso";
	}
	return "other";
}

template <typename Map>
std::vector<std::pair<std::string, uint64_t>> Top(const Map& counts, size_t limit) {
	std::vector<std::pair<std::string, uint64_t>> sorted(counts.begin(), counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	if (sorted.size() > limit) {
		sorted.resize(limit);
	}
	return sorted;
}

struct Window {
	std::unordered_map<int, uint64_t>         per_thread;
	std::map<std::string, uint64_t>           per_category;
	uint64_t                                  total = 0;
};

struct Profile {
	std::unordered_map<uint64_t, uint64_t> leaf;      // pc -> samples
	std::unordered_map<uint64_t, uint64_t> kyty;      // innermost libmain frame -> samples
	std::unordered_map<int, uint64_t>      threads;   // tid -> samples
	std::unordered_map<uint32_t, uint64_t> hle;       // HLE id -> calls
	std::map<std::string, uint64_t>        wall;      // wall-clock samples: leaf + kyty frames
	std::map<std::string, uint64_t>        stacks;    // CPU samples: leaf + kyty frames
	uint64_t                               wall_total = 0;
	uint64_t                               total = 0;
};

void DumpProfile(const Profile& profile, Symbolizer& symbolizer, int seconds) {
	FILE* out = std::fopen("_perf_profile.txt", "w");
	if (out == nullptr) {
		return;
	}
	std::fprintf(out, "# last %d s, %llu samples (1 sample = 1 ms thread CPU time)\n", seconds,
	             static_cast<unsigned long long>(profile.total));
	std::fprintf(out, "## threads\n");
	std::map<std::string, uint64_t> threads;
	for (const auto& [tid, n]: profile.threads) {
		threads[std::to_string(tid) + ":" + ThreadName(tid)] += n;
	}
	for (const auto& [name, n]: Top(threads, kTopEntries)) {
		std::fprintf(out, "%llu %s\n", static_cast<unsigned long long>(n), name.c_str());
	}
	auto dump = [&](const char* title, const std::unordered_map<uint64_t, uint64_t>& counts) {
		std::map<std::string, uint64_t> located;
		for (const auto& [pc, n]: counts) {
			located[symbolizer.Locate(pc)] += n;
		}
		std::fprintf(out, "## %s\n", title);
		for (const auto& [where, n]: Top(located, kTopEntries * 4)) {
			std::fprintf(out, "%llu %s\n", static_cast<unsigned long long>(n), where.c_str());
		}
	};
	dump("leaf", profile.leaf);
	if (auto resolver = GetHleAddressResolver(); resolver != nullptr) {
		std::unordered_map<uint64_t, uint64_t> by_thunk;
		for (const auto& [id, n]: profile.hle) {
			by_thunk[resolver(id)] += n;
		}
		dump("hle-calls", by_thunk);
	}
	dump("innermost-kyty-frame", profile.kyty);
	std::fprintf(out, "## cpu-stacks (leaf <- kyty callers)\n");
	for (const auto& [stack, n]: Top(profile.stacks, kTopEntries)) {
		std::fprintf(out, "%llu %s\n", static_cast<unsigned long long>(n), stack.c_str());
	}
	std::fprintf(out, "## wall-stacks (%llu wall-clock samples of 1 ms: leaf <- kyty callers)\n",
	             static_cast<unsigned long long>(profile.wall_total));
	for (const auto& [stack, n]: Top(profile.wall, kTopEntries)) {
		std::fprintf(out, "%llu %s\n", static_cast<unsigned long long>(n), stack.c_str());
	}
	std::fclose(out);
}

void Reporter() {
	Symbolizer   symbolizer;
	ThreadTimers timers;
	Profile      profile;
	uint64_t     read_index   = 0;
	uint64_t     last_hle     = 0;
	uint64_t     last_faults  = 0;
	uint64_t     last_flips   = 0;
	uint64_t     last_audio   = 0;
	int          seconds      = 0;
	auto         last_time    = std::chrono::steady_clock::now();
	std::unordered_map<int, std::string> wall_names;

	for (;;) {
		timers.Sync();
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto now     = std::chrono::steady_clock::now();
		const double dt    = std::chrono::duration<double>(now - last_time).count();
		last_time          = now;

		symbolizer.NewPeriod();
		Window         window;
		const uint64_t write = g_write.load(std::memory_order_acquire);
		if (write - read_index > kRingSize) {
			read_index = write - kRingSize;
		}
		for (; read_index < write; read_index++) {
			const Sample& s = g_ring[read_index & (kRingSize - 1)];
			if (s.seq.load(std::memory_order_acquire) != read_index + 1) {
				continue;
			}
			const uint64_t pc = s.frames[0];
			const uint64_t w  = s.weight;
			const auto stack_of = [&](std::string stack) {
				stack += symbolizer.Locate(pc);
				size_t      kyty  = 0;
				for (uint32_t i = 1; i < s.depth && kyty < kWallStackFrames; i++) {
					if (std::strcmp(Category(symbolizer.Module(s.frames[i])), "kyty") == 0) {
						stack += " <- " + symbolizer.Locate(s.frames[i]);
						kyty++;
					}
				}
				return stack;
			};
			if (s.wall) {
				auto& thread_name = wall_names[s.tid];
				if (thread_name.empty()) {
					thread_name = ThreadName(s.tid) + ": ";
				}
				profile.wall[stack_of(thread_name)] += w;
				profile.wall_total += w;
				continue;
			}
			profile.stacks[stack_of({})] += w;
			window.per_thread[s.tid] += w;
			window.per_category[Category(symbolizer.Module(pc))] += w;
			window.total += w;
			profile.leaf[pc] += w;
			profile.threads[s.tid] += w;
			profile.total += w;
			for (uint32_t i = 0; i < s.depth; i++) {
				if (std::strcmp(Category(symbolizer.Module(s.frames[i])), "kyty") == 0) {
					profile.kyty[s.frames[i]] += w;
					break;
				}
			}
		}

		for (uint32_t id = 0; id < kMaxHleIds; id++) {
			if (const uint32_t n = Detail::g_hle_by_id[id].exchange(0, std::memory_order_relaxed); n != 0) {
				profile.hle[id] += n;
			}
		}
		const uint64_t hle    = Detail::g_hle_calls.load(std::memory_order_relaxed);
		const uint64_t faults = Detail::g_gpu_faults.load(std::memory_order_relaxed);
		const uint64_t flips  = Detail::g_flips.load(std::memory_order_relaxed);
		const uint64_t audio  = Detail::g_audio_underruns.load(std::memory_order_relaxed);

		char head[256];
		std::snprintf(head, sizeof(head),
		              "[perf] flips/s=%.1f hle/s=%.0f gpu_faults/s=%.0f audio_underruns=%llu cpu=%.0f%% |",
		              static_cast<double>(flips - last_flips) / dt, static_cast<double>(hle - last_hle) / dt,
		              static_cast<double>(faults - last_faults) / dt,
		              static_cast<unsigned long long>(audio - last_audio),
		              static_cast<double>(window.total) / (dt * 10.0));
		std::string line = head;
		for (const auto& [category, n]: Top(window.per_category, 8)) {
			char part[64];
			std::snprintf(part, sizeof(part), " %s=%.0f%%", category.c_str(),
			              static_cast<double>(n) / (dt * 10.0));
			line += part;
		}
		line += " | " + TakeFramePercentiles() + " " + TakeFrameSummary(flips - last_flips) + " | busiest:";
		std::unordered_map<std::string, uint64_t> threads;
		for (const auto& [tid, n]: window.per_thread) {
			threads[std::to_string(tid) + "/" + ThreadName(tid)] = n;
		}
		for (const auto& [name, n]: Top(threads, 4)) {
			char part[96];
			std::snprintf(part, sizeof(part), " %s=%.0f%%", name.c_str(), static_cast<double>(n) / (dt * 10.0));
			line += part;
		}
		Emit(line);
		Emit(TakeWaitSummary(dt, flips - last_flips));
		Emit(TakeReadbackSummary(dt, flips - last_flips));

		last_hle    = hle;
		last_faults = faults;
		last_flips  = flips;
		last_audio  = audio;
		if (++seconds % kProfileDumpSeconds == 0) {
			DumpProfile(profile, symbolizer, kProfileDumpSeconds);
			profile = {};
		}
	}
}

} // namespace

void SampleThisThreadWallClockIfNamed(const std::string& name) {
	static const std::string filter = [] {
		const char* value = std::getenv("KYTY_PERF_WALL_THREADS");
		return value != nullptr ? std::string(value) : std::string();
	}();
	if (filter.empty() || name.empty()) {
		return;
	}
	size_t begin = 0;
	while (begin <= filter.size()) {
		const size_t end   = filter.find_first_of(",;|", begin);
		const auto   token = filter.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		if (!token.empty() && name.find(token) != std::string::npos) {
			SampleThisThreadWallClock();
			return;
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
}

void SampleThisThreadWallClock() {
	if (!Enabled()) {
		return;
	}
	sigevent sev {};
	sev.sigev_notify          = SIGEV_THREAD_ID;
	sev.sigev_signo           = kSampleSignal;
	sev.sigev_value.sival_int = kWallTag;
	sev._sigev_un._tid        = static_cast<pid_t>(syscall(SYS_gettid));
	int timer                 = -1;
	if (syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &timer) != 0) {
		return;
	}
	itimerspec spec {};
	spec.it_interval.tv_nsec = kSampleIntervalNs;
	spec.it_value.tv_nsec    = kSampleIntervalNs;
	syscall(SYS_timer_settime, timer, 0, &spec, nullptr);
}

void Start() {
	if (g_enabled.exchange(true)) {
		return;
	}
	struct sigaction sa {};
	sa.sa_sigaction = OnSample;
	sa.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(kSampleSignal, &sa, nullptr);
	sigset_t set {};
	sigemptyset(&set);
	sigaddset(&set, kSampleSignal);
	pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
	std::thread(Reporter).detach();
}

} // namespace Common::PerfStats

#else

namespace Common::PerfStats {

void SampleThisThreadWallClock() {}
void SampleThisThreadWallClockIfNamed(const std::string& /*name*/) {}

void Start() {
	Detail::g_enabled.store(true, std::memory_order_relaxed);
	std::fprintf(stderr, "[perf] sampling profiler is only available on Linux/Android\n");
}

} // namespace Common::PerfStats

#endif
