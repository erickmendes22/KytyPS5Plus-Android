#ifndef EMULATOR_SRC_KERNEL_THREADPLACEMENT_H_
#define EMULATOR_SRC_KERNEL_THREADPLACEMENT_H_

#include <cstdint>
#include <string_view>

// Host placement of the threads that gate frame submission (Android/Linux; inert elsewhere).
//
// KYTY_CPU_PLACE=<bits> (start value; the live knob cpu_place=<bits> overrides it while a game
// runs with KYTY_LIVE_KNOBS=1; unset = kAndroidDefaultMode on Android, 0 elsewhere):
//   1  pin the critical threads to the cores with the highest current frequency limit
//      (scaling_max_freq, re-read every second; a hot Snapdragon 8 Elite caps its 4.32 GHz prime
//      cores below its 3.53 GHz cores)
//   2  an ADPF performance-hint session over the critical threads, told every frame that the
//      frame took longer than its target, so the scheduler raises their clocks
//   4  keep every other thread of the process off the fastest cores (with 1)
//   8  split: the critical threads get the prime cluster (the cores with the highest hardware
//      maximum, cpuinfo_max_freq: cpu6-7 on a Snapdragon 8 Elite) to themselves, every other
//      thread of the process (guest threads, SRT walk workers, ...) the remaining cores; overrides
//      1 and 4. The two serial stages of the GPU front-end then run in their own frequency domain
//      and share its L2, instead of sharing cpu0-5's clock with ~3 cores of polling guest work
//      while the prime cores idle (c17_pow1). The Android default together with KYTY_PIPE_SPIN
//      (pipeSpin.h; alone, either one gains nothing): Astro Bot +15% at the same GPU clock step
//      (g5_astro_ab3: 14.38 -> 16.53 fps). KYTY_CPU_PLACE=0 opts out.
// Critical threads: the GPU command-processor thread (the front), the KYTY_GPU_PIPELINE record
// thread, plus guest threads whose name contains one of the substrings in KYTY_CPU_CRITICAL (','
// ';' or '|' separated).
namespace Kernel::ThreadPlacement {

inline constexpr int kModeBits = 15;
#if defined(__ANDROID__)
inline constexpr int kAndroidDefaultMode = 8;
#else
inline constexpr int kAndroidDefaultMode = 0;
#endif

enum class Role : uint8_t {
	GpuCommand, // GuestGpu::ThreadRun
	GpuRecord,  // GpuPipeline::RecordThread (critical)
	Guest,      // a named guest pthread (critical only if its name matches KYTY_CPU_CRITICAL)
};

// Records the calling thread and applies the current placement to it.
void RegisterCurrentThread(Role role, std::string_view name = {});

// Re-applies the placement when `mode` (the live value, -1 = keep the start value) changed.
// Called once per processed frame by the GPU thread; cheap when nothing changed.
void Poll(int64_t mode);

// ADPF: one frame's wall time, reported to the hint session when bit 2 is on.
void ReportFrame(int64_t frame_ns);

// Parses a KYTY_CPU_PLACE value (decimal bits 0..15); -1 for anything else. Exposed for tests.
[[nodiscard]] int ParseMode(std::string_view text);

// The start mode from the environment string (nullptr = unset): unset = kAndroidDefaultMode on
// Android, 0 elsewhere; an invalid value = 0.
[[nodiscard]] int StartModeFrom(const char* value, bool android);

// The prime cluster from per-core hardware maxima (kHz, <= 0 = unknown): bit i set for every core
// at the highest maximum. 0 when every known core has the same maximum (nothing to split), when
// no maximum is known, or for more than 64 cores.
[[nodiscard]] constexpr uint64_t PrimeMask(const long* max_khz, int count) {
	if (count <= 0 || count > 64) return 0;
	long best = 0;
	for (int i = 0; i < count; i++) best = max_khz[i] > best ? max_khz[i] : best;
	if (best <= 0) return 0;
	uint64_t mask  = 0;
	int      known = 0;
	for (int i = 0; i < count; i++) {
		if (max_khz[i] > 0) known++;
		if (max_khz[i] == best) mask |= uint64_t {1} << static_cast<unsigned>(i);
	}
	int primes = 0;
	for (uint64_t m = mask; m != 0; m &= m - 1) primes++;
	return primes == known ? 0 : mask;
}

} // namespace Kernel::ThreadPlacement

#endif // EMULATOR_SRC_KERNEL_THREADPLACEMENT_H_
