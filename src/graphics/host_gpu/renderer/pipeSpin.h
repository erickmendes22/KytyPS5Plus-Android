#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPESPIN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPESPIN_H_

#include <atomic>
#include <chrono>
#include <cstdint>

// KYTY_PIPE_SPIN=<us> (live key pipe_spin): the GPU front/record/walk handoffs spin for up to
// <us> microseconds on an atomic before they sleep on a condition variable. Every front-record
// round trip (~150 drains and ~670 items per Astro Bot frame) otherwise pays two futex sleeps and
// two wake-ups on the critical path: a syscall each plus the scheduler's wake latency on a core
// that dropped into idle. 0 = off (sleep at once, the previous behaviour). Unset: kAndroidSpinUs on
// Android, 0 elsewhere. The spin is bounded, so it costs at most <us> of CPU per wait.
// Measured with Astro Bot's performance preset, in-run A/Bs at the same (throttled) GPU clock step:
// on its own it gains nothing (g5_astro_ab1: 14.96 -> 15.06 fps), and split placement on its own
// (KYTY_CPU_PLACE=8, threadPlacement.h) gains nothing either (g5_astro_ab2: 15.23 -> 14.69), but
// the two together give +15% (g5_astro_ab3, ABABAB: 14.38 -> 16.53 fps, frame p50 66.7 -> 58.6
// ms): the front and record threads then own the prime cores and hand off without letting them
// idle down between items.
namespace Libs::Graphics::PipeSpin {

#if defined(__ANDROID__)
inline constexpr bool kAndroid = true;
#else
inline constexpr bool kAndroid = false;
#endif

inline constexpr int64_t kAndroidSpinUs = 60;
inline constexpr int64_t kMaxSpinUs     = 2000;

// The start value from the environment string (nullptr = unset). Digits only; anything else,
// or a value above kMaxSpinUs, is clamped (text = 0, large = kMaxSpinUs).
[[nodiscard]] constexpr int64_t SpinMicros(const char* value, bool android) {
	if (value == nullptr) return android ? kAndroidSpinUs : 0;
	int64_t n = 0;
	for (; *value >= '0' && *value <= '9'; ++value) {
		n = n * 10 + (*value - '0');
		if (n > kMaxSpinUs) return kMaxSpinUs;
	}
	return n;
}

// The live value: `live` < 0 means no live key (keep `start`). Clamped to [0, kMaxSpinUs].
[[nodiscard]] constexpr int64_t Effective(int64_t start, int64_t live) {
	const int64_t value = live < 0 ? start : live;
	return value < 0 ? 0 : (value > kMaxSpinUs ? kMaxSpinUs : value);
}

inline void Relax() {
#if defined(__aarch64__)
	asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
	__builtin_ia32_pause();
#else
	std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Spins until `done()` or `micros` elapsed; true if `done()` became true. micros <= 0: one check.
// The clock is read every 64 polls only (a vDSO call each).
template <typename Done>
[[nodiscard]] bool SpinFor(int64_t micros, Done&& done) {
	if (done()) return true;
	if (micros <= 0) return false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
	for (uint32_t i = 1;; i++) {
		Relax();
		if (done()) return true;
		if ((i & 63u) == 0 && std::chrono::steady_clock::now() >= deadline) return false;
	}
}

// The current spin in microseconds (KYTY_PIPE_SPIN + live pipe_spin). Defined in
// gpuPipeline.cpp; every call also refreshes Cached().
[[nodiscard]] int64_t Current();

// The value of the last Current() call (0 before the first): for code built without the renderer
// (walkFan.cpp in walk_fan_tests).
inline std::atomic<int64_t> g_cached_us {0};
[[nodiscard]] inline int64_t Cached() {
	return g_cached_us.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics::PipeSpin

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPESPIN_H_
