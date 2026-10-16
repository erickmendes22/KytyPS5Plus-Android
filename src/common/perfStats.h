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

inline void CountHleCall() {
	if (Enabled()) {
		Detail::g_hle_calls.fetch_add(1, std::memory_order_relaxed);
	}
}

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

// Touch-to-guest latency probe. MarkInput is called when the event loop handles a press that
// SDL queued queue_delay_ns earlier; ObserveInput when the guest reads pad state with that press
// visible. The first observation after a mark logs both delays.
void MarkInput(uint64_t queue_delay_ns);
void ObserveInput();

// Starts the reporter thread and the sampler. Call once, early on the main thread, before the
// threads to be profiled are created (they inherit the unblocked profiling signal).
void Start();

} // namespace Common::PerfStats

#endif /* KYTY_COMMON_PERFSTATS_H_ */
