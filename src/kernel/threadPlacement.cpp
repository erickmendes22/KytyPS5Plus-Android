#include "kernel/threadPlacement.h"

#include <charconv>
#include <cstdlib>
#include <string>

#if defined(__linux__) || defined(__ANDROID__)

#include "common/logging/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <dlfcn.h>
#include <mutex>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace Kernel::ThreadPlacement {

namespace {

constexpr int     kPin       = 1;
constexpr int     kHint      = 2;
constexpr int     kExclusive = 4;
constexpr int     kSplit     = 8;
constexpr int     kMaxCpus   = 64;
constexpr int64_t kPollNs    = 1'000'000'000;
// ADPF target: a 30 fps frame. Astro Bot gameplay runs slower, so every report asks for more.
constexpr int64_t kHintTargetNs = 33'333'333;

struct Cpus {
	cpu_set_t all {};
	cpu_set_t fast {};
	cpu_set_t rest {};
	int       fast_count = 0;
	long      best_khz   = 0;
	// kSplit: the prime cluster (highest cpuinfo_max_freq) and the other cores.
	cpu_set_t prime {};
	cpu_set_t non_prime {};
	int       prime_count = 0;
};

// ADPF (API 33+), resolved at run time: the app's minSdk is 31.
struct HintApi {
	using GetManager  = void* (*)();
	using Create      = void* (*)(void*, const int32_t*, size_t, int64_t);
	using Report      = int (*)(void*, int64_t);
	using Close       = void (*)(void*);
	GetManager get_manager = nullptr;
	Create     create      = nullptr;
	Report     report      = nullptr;
	Close      close       = nullptr;
	bool       loaded      = false;
};

std::mutex           g_mutex;
std::vector<int32_t> g_critical;
std::atomic<int>     g_mode {-1};
int                  g_applied_mode = 0;
size_t               g_applied_critical = 0;
int64_t              g_next_poll_ns     = 0;
bool                 g_others_split     = false; // non-critical threads are kept off the prime cores
std::atomic<void*>   g_session {nullptr};
HintApi              g_hint;

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

int32_t Tid() {
	return static_cast<int32_t>(syscall(SYS_gettid));
}

long ReadFreq(int cpu, const char* file) {
	char path[96];
	std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, file);
	std::FILE* f = std::fopen(path, "r");
	if (f == nullptr) {
		return -1;
	}
	long value = -1;
	if (std::fscanf(f, "%ld", &value) != 1) {
		value = -1;
	}
	std::fclose(f);
	return value;
}

// The cores whose current frequency limit (scaling_max_freq: thermal mitigation lowers it, and
// on a hot Snapdragon 8 Elite the 4.32 GHz prime cores are capped below the 3.53 GHz cores) is
// the highest. Re-read on every poll.
Cpus ReadCpus() {
	Cpus result;
	CPU_ZERO(&result.all);
	CPU_ZERO(&result.fast);
	CPU_ZERO(&result.rest);
	long       freq[kMaxCpus] {};
	long       hw_max[kMaxCpus] {};
	long       best   = -1;
	const long online = sysconf(_SC_NPROCESSORS_CONF);
	const int  count  = online > 0 && online < kMaxCpus ? static_cast<int>(online) : 0;
	for (int cpu = 0; cpu < count; cpu++) {
		freq[cpu] = ReadFreq(cpu, "scaling_max_freq");
		if (freq[cpu] <= 0) {
			freq[cpu] = ReadFreq(cpu, "cpuinfo_max_freq");
		}
		best = freq[cpu] > best ? freq[cpu] : best;
		CPU_SET(cpu, &result.all);
		hw_max[cpu] = ReadFreq(cpu, "cpuinfo_max_freq");
	}
	CPU_ZERO(&result.prime);
	CPU_ZERO(&result.non_prime);
	const uint64_t prime = PrimeMask(hw_max, count);
	for (int cpu = 0; cpu < count && prime != 0; cpu++) {
		if (((prime >> static_cast<unsigned>(cpu)) & 1u) != 0) {
			CPU_SET(cpu, &result.prime);
			result.prime_count++;
		} else {
			CPU_SET(cpu, &result.non_prime);
		}
	}
	for (int cpu = 0; cpu < count; cpu++) {
		if (freq[cpu] == best && best > 0) {
			CPU_SET(cpu, &result.fast);
			result.fast_count++;
		} else {
			CPU_SET(cpu, &result.rest);
		}
	}
	// Uniform limits: nothing to pin to.
	if (result.fast_count == count) {
		CPU_ZERO(&result.fast);
		result.fast_count = 0;
	}
	result.best_khz = best;
	return result;
}

Cpus g_cpus;

const Cpus& GetCpus() {
	return g_cpus;
}

void SetAffinity(int32_t tid, const cpu_set_t& set) {
	(void)sched_setaffinity(tid, sizeof(set), &set);
}

bool IsCritical(int32_t tid) {
	for (const auto id: g_critical) {
		if (id == tid) {
			return true;
		}
	}
	return false;
}

// Every thread of the process that is not critical: all cores, or `others` (exclusive).
void PlaceOthers(bool exclusive, const cpu_set_t& others) {
	const auto& cpus = GetCpus();
	DIR*        dir  = opendir("/proc/self/task");
	if (dir == nullptr) {
		return;
	}
	while (const auto* entry = readdir(dir)) {
		const auto tid = static_cast<int32_t>(std::strtol(entry->d_name, nullptr, 10));
		if (tid > 0 && !IsCritical(tid)) {
			SetAffinity(tid, exclusive ? others : cpus.all);
		}
	}
	closedir(dir);
}

void LoadHintApi() {
	if (g_hint.loaded) {
		return;
	}
	g_hint.loaded = true;
	void* lib     = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
	if (lib == nullptr) {
		return;
	}
	g_hint.get_manager = reinterpret_cast<HintApi::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
	g_hint.create      = reinterpret_cast<HintApi::Create>(dlsym(lib, "APerformanceHint_createSession"));
	g_hint.report = reinterpret_cast<HintApi::Report>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
	g_hint.close  = reinterpret_cast<HintApi::Close>(dlsym(lib, "APerformanceHint_closeSession"));
}

void CloseSession() {
	auto* session = g_session.exchange(nullptr);
	if (session != nullptr && g_hint.close != nullptr) {
		g_hint.close(session);
	}
}

void OpenSession() {
	LoadHintApi();
	if (g_hint.get_manager == nullptr || g_hint.create == nullptr || g_hint.report == nullptr ||
	    g_critical.empty()) {
		LOGF("ThreadPlacement: ADPF hint session unavailable\n");
		return;
	}
	void* manager = g_hint.get_manager();
	void* session = manager != nullptr
	                    ? g_hint.create(manager, g_critical.data(), g_critical.size(), kHintTargetNs)
	                    : nullptr;
	LOGF("ThreadPlacement: ADPF hint session over %zu threads: %s\n", g_critical.size(),
	     session != nullptr ? "ok" : "failed");
	g_session.store(session);
}

// Caller holds g_mutex.
void Apply(int mode) {
	const auto previous = g_cpus;
	g_cpus              = ReadCpus();
	const auto& cpus    = GetCpus();
	if (!CPU_EQUAL(&previous.fast, &cpus.fast) && (mode & kPin) != 0) {
		LOGF("ThreadPlacement: fastest cores now %d at %ld kHz\n", cpus.fast_count, cpus.best_khz);
	}
	const bool  split = (mode & kSplit) != 0 && cpus.prime_count > 0;
	const bool  pin   = !split && (mode & kPin) != 0 && cpus.fast_count > 0;
	for (const auto tid: g_critical) {
		SetAffinity(tid, split ? cpus.prime : (pin ? cpus.fast : cpus.all));
	}
	const bool exclusive = split || (pin && (mode & kExclusive) != 0);
	if (exclusive || (g_applied_mode & (kExclusive | kSplit)) != 0) {
		PlaceOthers(exclusive, split ? cpus.non_prime : cpus.rest);
	}
	g_others_split = split;
	const bool hint_now    = (mode & kHint) != 0;
	const bool hint_before = (g_applied_mode & kHint) != 0;
	if (hint_before && (!hint_now || g_applied_critical != g_critical.size())) {
		CloseSession();
	}
	if (hint_now && g_session.load() == nullptr) {
		OpenSession();
	}
	if (mode != g_applied_mode || g_applied_critical != g_critical.size()) {
		LOGF("ThreadPlacement: mode %d (pin=%d exclusive=%d hint=%d split=%d over %d prime cores) over %zu "
		     "critical threads\n",
		     mode, pin ? 1 : 0, exclusive ? 1 : 0, hint_now ? 1 : 0, split ? 1 : 0, cpus.prime_count,
		     g_critical.size());
	}
	g_applied_mode     = mode;
	g_applied_critical = g_critical.size();
}

int StartMode() {
#if defined(__ANDROID__)
	constexpr bool kAndroid = true;
#else
	constexpr bool kAndroid = false;
#endif
	return StartModeFrom(std::getenv("KYTY_CPU_PLACE"), kAndroid);
}

bool NameIsCritical(std::string_view name) {
	static const std::string filter = [] {
		const char* value = std::getenv("KYTY_CPU_CRITICAL");
		return value != nullptr ? std::string(value) : std::string();
	}();
	if (filter.empty() || name.empty()) {
		return false;
	}
	size_t begin = 0;
	while (begin <= filter.size()) {
		const size_t end   = filter.find_first_of(",;|", begin);
		const auto   token = std::string_view(filter).substr(begin, end == std::string::npos ? std::string::npos
		                                                                                      : end - begin);
		if (!token.empty() && name.find(token) != std::string_view::npos) {
			return true;
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return false;
}

} // namespace

void RegisterCurrentThread(Role role, std::string_view name) {
	const bool critical = role == Role::GpuCommand || role == Role::GpuRecord || NameIsCritical(name);
	std::lock_guard lock(g_mutex);
	if (g_mode.load() < 0) {
		g_mode.store(StartMode());
	}
	const auto tid = Tid();
	if (critical) {
		if (!IsCritical(tid)) {
			g_critical.push_back(tid);
		}
		LOGF("ThreadPlacement: critical thread %d (%.*s)\n", tid, static_cast<int>(name.size()), name.data());
		Apply(g_mode.load());
	} else if (g_others_split) {
		SetAffinity(tid, GetCpus().non_prime);
	} else if ((g_applied_mode & (kPin | kExclusive)) == (kPin | kExclusive)) {
		SetAffinity(tid, GetCpus().rest);
	}
}

void Poll(int64_t mode) {
	const auto now = NowNs();
	std::lock_guard lock(g_mutex);
	if (now < g_next_poll_ns) {
		return;
	}
	g_next_poll_ns = now + kPollNs;
	// No live value: back to the start value (a knob phase without cpu_place).
	g_mode.store(mode >= 0 && mode <= kModeBits ? static_cast<int>(mode) : StartMode());
	const int wanted = g_mode.load();
	// Exclusive placement is re-applied every poll: threads created since inherit their creator's set.
	// With pinning on, re-applied every poll: the fastest cores follow the thermal limits, and
	// threads created since inherit their creator's set.
	if (wanted != g_applied_mode || g_applied_critical != g_critical.size() || (wanted & (kPin | kSplit)) != 0) {
		Apply(wanted);
	}
}

void ReportFrame(int64_t frame_ns) {
	if (frame_ns <= 0 || g_session.load(std::memory_order_relaxed) == nullptr) {
		return;
	}
	// Under the lock: a mode change may close the session meanwhile.
	std::lock_guard lock(g_mutex);
	if (auto* session = g_session.load(); session != nullptr) {
		(void)g_hint.report(session, frame_ns);
	}
}

int ParseMode(std::string_view text) {
	int value = -1;
	const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && ptr == text.data() + text.size() && value >= 0 && value <= kModeBits ? value : -1;
}

int StartModeFrom(const char* value, bool android) {
	if (value == nullptr) {
		return android ? kAndroidDefaultMode : 0;
	}
	const int mode = ParseMode(value);
	return mode >= 0 ? mode : 0;
}

} // namespace Kernel::ThreadPlacement

#else

namespace Kernel::ThreadPlacement {

void RegisterCurrentThread(Role /*role*/, std::string_view /*name*/) {}
void Poll(int64_t /*mode*/) {}
void ReportFrame(int64_t /*frame_ns*/) {}

int ParseMode(std::string_view text) {
	int value = -1;
	const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && ptr == text.data() + text.size() && value >= 0 && value <= kModeBits ? value : -1;
}

int StartModeFrom(const char* value, bool android) {
	if (value == nullptr) {
		return android ? kAndroidDefaultMode : 0;
	}
	const int mode = ParseMode(value);
	return mode >= 0 ? mode : 0;
}

} // namespace Kernel::ThreadPlacement

#endif
