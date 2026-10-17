#include "graphics/host_gpu/gpuMemoryBudget.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics::GpuBudget;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "GpuMemoryBudgetTests: failed: %s\n", text);
		std::abort();
	}
}

void TestAutoBudget() {
	Check(AutoBudget(0) == kAutoCap, "unknown MemAvailable uses the cap");
	Check(AutoBudget(uint64_t {9333} * MiB) == kAutoCap, "OnePlus Pad 3, 9.1 GiB available -> cap");
	Check(AutoBudget(uint64_t {8929604} * 1024) == kAutoCap, "8.5 GiB available (idle) -> cap");
	Check(AutoBudget(8 * GiB) == 4 * GiB, "8 GiB available -> 4 GiB");
	Check(AutoBudget(uint64_t {6109} * MiB) == kAutoFloor, "6 GiB available -> floor");
	Check(AutoBudget(5 * GiB) == kAutoFloor, "never below the floor");
	Check(AutoBudget(1 * GiB) == kAutoFloor, "reserve larger than available -> floor");
	Check(AutoBudget(64 * GiB) == kAutoCap, "never above the cap");
}

void TestThresholds() {
	const auto t = ForBudget(4 * GiB); // also what --gpu-memory-budget 4096 gives
	Check(t.trigger < t.pressure && t.pressure < t.critical && t.critical < 4 * GiB,
	      "thresholds are ordered and below the budget");
	Check(t.trigger / MiB == 2252 && t.pressure / MiB == 3276 && t.critical / MiB == 3768,
	      "4 GiB -> 2252/3276/3768 MiB");
	const auto zero = ForBudget(0);
	Check(zero.trigger == 0 && zero.pressure == 0 && zero.critical == 0, "zero budget");
}

void TestEffective() {
	Check(Effective(4 * GiB, 3 * GiB, 0) == 4 * GiB, "no driver budget keeps the configured one");
	Check(Effective(0, 3 * GiB, 8 * GiB) == 0, "disabled stays disabled");
	Check(Effective(4 * GiB, 3 * GiB, 8 * GiB) == 4 * GiB, "plenty of headroom");
	Check(Effective(4 * GiB, 4 * GiB, 4 * GiB + kLowWater) == 4 * GiB, "exactly the low water");
	// 512 MiB headroom: 256 MiB short of the low water, so the budget drops to usage - 256 MiB.
	Check(Effective(4 * GiB, 3 * GiB, 3 * GiB + 512 * MiB) == 3 * GiB - 256 * MiB,
	      "low headroom shrinks below current usage");
	Check(Effective(4 * GiB, 3 * GiB, 4 * GiB + 512 * MiB) == 4 * GiB,
	      "1.5 GiB headroom (normal while emulating) keeps the budget");
	Check(Effective(4 * GiB, 2 * GiB, 2 * GiB) == kRuntimeFloor, "never below the runtime floor");
	Check(Effective(1 * GiB, 2 * GiB, 2 * GiB) == 1 * GiB, "floor never exceeds the configured budget");
	Check(Effective(4 * GiB, 6 * GiB, 6 * GiB + 256 * MiB) == 4 * GiB,
	      "never above the configured budget");
}

} // namespace

int main() {
	TestAutoBudget();
	TestThresholds();
	TestEffective();
	std::printf("GpuMemoryBudgetTests: ok\n");
	return 0;
}
