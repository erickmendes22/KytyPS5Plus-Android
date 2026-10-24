#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "common/processMemory.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

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

void TestUnifiedSystemPressure() {
	// CPU/guest allocations consume shared RAM even if the driver's estimate lags.
	Check(Effective(4 * GiB, 3 * GiB, 8 * GiB, 512 * MiB) == 3 * GiB - 256 * MiB,
	      "system pressure overrides optimistic driver headroom");
	Check(Effective(4 * GiB, 3 * GiB, 0, 512 * MiB) == 3 * GiB - 256 * MiB,
	      "system pressure works without VK_EXT_memory_budget");
	Check(Effective(4 * GiB, 3 * GiB, 8 * GiB, 0) == 3 * GiB - kLowWater,
	      "zero available RAM is real pressure, not a missing sample");
	Check(Effective(4 * GiB, 3 * GiB, 0) == 4 * GiB,
	      "unknown driver and system budgets preserve existing policy");
	Check(Effective(0, 3 * GiB, 8 * GiB, 0) == 0, "explicitly disabled stays disabled");
	Check(Effective(4 * GiB, 3 * GiB, 8 * GiB, kLowWater) == 4 * GiB,
	      "healthy system headroom keeps existing budget");
	Check(Effective(4 * GiB, 3 * GiB, 3 * GiB + 256 * MiB, 8 * GiB) ==
	          3 * GiB - 512 * MiB,
	      "healthy system cannot hide driver pressure");
	Check(Effective(1 * GiB, 0, 0, 0) == 1 * GiB,
	      "no unsigned underflow and floor does not exceed configured budget");
}

void TestProcessMemoryReadings() {
	std::istringstream meminfo("MemTotal: 12000000 kB\nMemAvailable:\t512000 kB\n");
	std::istringstream status("Name:\tkyty\nVmRSS: 6000000 kB\nRssAnon: 4000000 kB\nVmSwap: 0 kB\n");
	const auto sample = Common::ParseProcessMemory(meminfo, status);
	Check(sample.available == uint64_t{512000} * 1024, "available RAM parsed in bytes");
	Check(sample.resident == uint64_t{6000000} * 1024, "RSS parsed after nonnumeric fields");
	Check(sample.anonymous == uint64_t{4000000} * 1024, "anonymous RAM separated from RSS");
	Check(sample.swap && *sample.swap == 0, "zero swap remains a valid reading");
	std::istringstream empty_meminfo, empty_status;
	const auto missing = Common::ParseProcessMemory(empty_meminfo, empty_status);
	Check(!missing.available && !missing.resident && !missing.anonymous && !missing.swap,
	      "unavailable proc files do not imply zero RAM");
	Check(!Common::ParseProcMemoryKiB("VmRSS: 18446744073709551615 kB", "VmRSS:"),
	      "byte conversion overflow rejected");
	Check(!Common::ParseProcMemoryKiB("VmRSS: invalid kB", "VmRSS:"), "invalid number rejected");
	Check(!Common::ParseProcMemoryKiB("VmRSS: 10 MB", "VmRSS:"), "unexpected unit rejected");
	Check(!Common::ParseProcMemoryKiB("VmRSS: 10 kB extra", "VmRSS:"), "trailing text rejected");
}

} // namespace

int main() {
	TestAutoBudget();
	TestThresholds();
	TestEffective();
	TestUnifiedSystemPressure();
	TestProcessMemoryReadings();
	std::printf("GpuMemoryBudgetTests: ok\n");
	return 0;
}
