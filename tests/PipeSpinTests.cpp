// pipe_spin_tests: KYTY_PIPE_SPIN start values and the bounded spin (pipeSpin.h), and the prime
// cluster selection of KYTY_CPU_PLACE's split mode (threadPlacement.h).
#include "graphics/host_gpu/renderer/pipeSpin.h"
#include "kernel/threadPlacement.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

namespace PS = Libs::Graphics::PipeSpin;
namespace TP = Kernel::ThreadPlacement;

// Unset: on with the measured value on Android, off elsewhere; an explicit value wins (=0 opts out).
static_assert(PS::SpinMicros(nullptr, true) == PS::kAndroidSpinUs);
static_assert(PS::kAndroidSpinUs == 60);
static_assert(PS::SpinMicros(nullptr, false) == 0);
static_assert(PS::SpinMicros("60", true) == 60);
static_assert(PS::SpinMicros("0", true) == 0);
static_assert(PS::SpinMicros("25", false) == 25);
static_assert(PS::SpinMicros("abc", true) == 0);
static_assert(PS::SpinMicros("", true) == 0);
static_assert(PS::SpinMicros("-5", true) == 0);
static_assert(PS::SpinMicros("999999999999999999999", true) == PS::kMaxSpinUs);

// Live values: -1 = no key (keep the start value); clamped.
static_assert(PS::Effective(60, -1) == 60);
static_assert(PS::Effective(60, 0) == 0);
static_assert(PS::Effective(0, 40) == 40);
static_assert(PS::Effective(0, 1'000'000) == PS::kMaxSpinUs);

// Snapdragon 8 Elite: six 3.53 GHz cores and two 4.32 GHz prime cores.
constexpr long kElite[8] = {3532800, 3532800, 3532800, 3532800, 3532800, 3532800, 4320000, 4320000};
static_assert(TP::PrimeMask(kElite, 8) == 0xc0u);
// Uniform cores: nothing to split.
constexpr long kUniform[4] = {2000000, 2000000, 2000000, 2000000};
static_assert(TP::PrimeMask(kUniform, 4) == 0);
// Unknown cores are not counted as non-prime.
constexpr long kPartial[4] = {-1, 2000000, 2500000, -1};
static_assert(TP::PrimeMask(kPartial, 4) == 0x4u);
constexpr long kUnknown[2] = {-1, 0};
static_assert(TP::PrimeMask(kUnknown, 2) == 0);
constexpr long kOneKnown[3] = {-1, 1800000, -1};
static_assert(TP::PrimeMask(kOneKnown, 3) == 0);
static_assert(TP::PrimeMask(kElite, 0) == 0);
static_assert(TP::PrimeMask(kElite, 65) == 0);
static_assert(TP::kModeBits == 15);
// Split placement is the Android default (with KYTY_PIPE_SPIN: +15% fps at the same GPU clock step).
static_assert(TP::kAndroidDefaultMode == (PS::kAndroid ? 8 : 0));

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PipeSpinTests: failed: %s\n", message);
		g_failures++;
	}
}

void TestSpinFor() {
	// Already true: no spin at all, even with a zero budget.
	Check(PS::SpinFor(0, [] { return true; }), "done at once");
	Check(!PS::SpinFor(0, [] { return false; }), "zero budget checks once");

	// A false condition gives up after about the budget.
	const auto start = std::chrono::steady_clock::now();
	Check(!PS::SpinFor(200, [] { return false; }), "never done");
	const auto spent = std::chrono::steady_clock::now() - start;
	Check(spent >= std::chrono::microseconds(200), "spins for the whole budget");
	Check(spent < std::chrono::milliseconds(50), "stops near the budget");

	// Another thread's release store ends the spin.
	std::atomic<bool> flag {false};
	int               payload = 0;
	std::thread       setter([&] {
		std::this_thread::sleep_for(std::chrono::microseconds(100));
		payload = 42;
		flag.store(true, std::memory_order_release);
	});
	const bool done = PS::SpinFor(PS::kMaxSpinUs * 100, [&] { return flag.load(std::memory_order_acquire); });
	setter.join();
	Check(done, "a concurrent store ends the spin");
	Check(payload == 42, "the store's data is visible");
	Check(PS::Cached() == 0, "no Current() call yet");
}

} // namespace

int main() {
	TestSpinFor();
	char value[] = "35";
	Check(PS::SpinMicros(value, false) == 35, "runtime spin value");
	if (g_failures != 0) return 1;
	std::printf("PipeSpinTests: ok\n");
	return 0;
}
