#include "common/perfStats.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace Common::PerfStats::Detail {

std::atomic<bool>     g_enabled {false};
std::atomic<uint64_t> g_hle_calls {0};
std::atomic<uint64_t> g_gpu_faults {0};
std::atomic<uint64_t> g_flips {0};
std::atomic<uint64_t> g_audio_underruns {0};

} // namespace Common::PerfStats::Detail

#include <chrono>

namespace Common::PerfStats {
namespace {

std::atomic<int64_t>  g_input_mark_ns {0};
std::atomic<uint64_t> g_input_queue_ns {0};

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

} // namespace

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
constexpr int      kProfileDumpSeconds = 10;
constexpr size_t   kTopEntries         = 80;

struct Sample {
	std::atomic<uint64_t> seq {0};
	int32_t               tid = 0;
	uint32_t              depth = 0;
	uint32_t              weight = 1; // expirations covered: CPU-time timers only fire on scheduler ticks
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
#if defined(__ANDROID__)
	__android_log_write(ANDROID_LOG_INFO, "KytyPerf", line.c_str());
#else
	std::fprintf(stderr, "%s\n", line.c_str());
#endif
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
	uint64_t                               total = 0;
};

void DumpProfile(const Profile& profile, Symbolizer& symbolizer, int seconds) {
	FILE* out = std::fopen("_perf_profile.txt", "w");
	if (out == nullptr) {
		return;
	}
	std::fprintf(out, "# %d s, %llu samples (1 sample = 1 ms thread CPU time)\n", seconds,
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
	dump("innermost-kyty-frame", profile.kyty);
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
		line += " | busiest:";
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

		last_hle    = hle;
		last_faults = faults;
		last_flips  = flips;
		last_audio  = audio;
		if (++seconds % kProfileDumpSeconds == 0) {
			DumpProfile(profile, symbolizer, seconds);
		}
	}
}

} // namespace

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

void Start() {
	Detail::g_enabled.store(true, std::memory_order_relaxed);
	std::fprintf(stderr, "[perf] sampling profiler is only available on Linux/Android\n");
}

} // namespace Common::PerfStats

#endif
