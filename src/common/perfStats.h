#ifndef KYTY_COMMON_PERFSTATS_H_
#define KYTY_COMMON_PERFSTATS_H_

// Opt-in performance counters and a CPU-time sampling profiler (--perf-stats). Off by default:
// every counter is a single predictable branch on a global flag until Start() runs.
//
// Once a second a "[perf]" line reports guest flips, HLE calls, GPU write-tracking faults and
// where the sampled CPU time went (FEX JIT code, Kyty, the Vulkan driver, libc, ...). Every
// kProfileDumpSeconds the hottest leaf addresses of that window are written to _perf_profile.txt as
// "module+offset" lines that llvm-symbolizer resolves against the unstripped libraries.

#include <atomic>
#include <string>
#include <cstdint>

namespace Common::PerfStats {

namespace Detail {
extern std::atomic<bool>     g_enabled;
extern std::atomic<uint64_t> g_hle_calls;
extern std::atomic<uint64_t> g_gpu_faults;
extern std::atomic<uint64_t> g_flips;
extern std::atomic<uint64_t> g_audio_underruns;
} // namespace Detail

inline bool Enabled() {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

constexpr uint32_t kMaxHleIds = 16384;
namespace Detail {
extern std::atomic<uint32_t> g_hle_by_id[kMaxHleIds];
} // namespace Detail

inline void CountHleCall(uint32_t id) {
	if (Enabled()) {
		Detail::g_hle_calls.fetch_add(1, std::memory_order_relaxed);
		if (id < kMaxHleIds) {
			Detail::g_hle_by_id[id].fetch_add(1, std::memory_order_relaxed);
		}
	}
}

// Maps an HLE id to its native thunk's address, so the busiest HLE functions can be named.
using HleAddressResolver = uintptr_t (*)(uint32_t id);
void SetHleAddressResolver(HleAddressResolver resolver);

inline void CountGpuFault() {
	if (Enabled()) {
		Detail::g_gpu_faults.fetch_add(1, std::memory_order_relaxed);
	}
}

// One presented guest flip: how long it waited since the guest submitted it, and how long the
// host present call blocked.
void CountFlip(uint64_t submit_to_present_us, uint64_t present_call_us);

// A primed audio output port found its host queue (nearly) drained: the listener hears a gap.
inline void CountAudioUnderrun() {
	if (Enabled()) {
		Detail::g_audio_underruns.fetch_add(1, std::memory_order_relaxed);
	}
}

// Timed one-off work that stalls the thread issuing it: host pipeline compilations
// (vkCreate*Pipelines) and guest shader translations (GCN -> SPIR-V -> shader module).
enum class StallKind { PipelineCompile, ShaderTranslate, Count };

void CountStall(StallKind kind, uint64_t micros);

class ScopedStall {
public:
	explicit ScopedStall(StallKind kind);
	~ScopedStall();
	ScopedStall(const ScopedStall&)            = delete;
	ScopedStall& operator=(const ScopedStall&) = delete;

private:
	StallKind kind_;
	int64_t   start_ns_ = 0;
};

// Touch-to-guest latency probe. MarkInput is called when the event loop handles a press that
// SDL queued queue_delay_ns earlier; ObserveInput when the guest reads pad state with that press
// visible. The first observation after a mark logs both delays.
void MarkInput(uint64_t queue_delay_ns);
void ObserveInput();

// Also samples the calling thread on wall-clock time, sleeping included, into the profile's
// "wall" sections: where the thread blocks. Meant for the GPU (command processor) thread.
void SampleThisThreadWallClock();

// SampleThisThreadWallClock for guest threads whose name contains one of the substrings in
// KYTY_PERF_WALL_THREADS, separated by ',', ';' or '|' (Android's "am start --esa" splits its
// list at commas, so use "Load|Streamer" there): shows where those threads block.
void SampleThisThreadWallClockIfNamed(const std::string& name);

// Starts the reporter thread and the sampler. Call once, early on the main thread, before the
// threads to be profiled are created (they inherit the unblocked profiling signal).
void Start();

} // namespace Common::PerfStats

#endif /* KYTY_COMMON_PERFSTATS_H_ */
