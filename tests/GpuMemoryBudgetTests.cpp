#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "common/processMemory.h"
#include "graphics/host_gpu/pipelineAllocationRecovery.h"

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

void TestPipelineAllocationRecovery() {
  using Libs::Graphics::RecoverPipelineAllocation;
  // Inject the exact Vulkan error from PID 14101 and device-memory exhaustion.
  // Track resource lifetime and GPU completion, not just the retry return code.
  for (int32_t initial : {-1, -2}) {
    for (int32_t retried : {0, -1, -2, -4}) {
      std::string events;
      bool gpu_in_flight = true;
      int failed_objects = 1, cached_objects = 3, attempts = 0;
      const auto result = RecoverPipelineAllocation(initial,
          [&] { Check(failed_objects == 1, "failed allocation is released once");
                failed_objects = 0; events += 'F'; },
          [&] { Check(failed_objects == 0, "failed objects freed before GPU drain");
                gpu_in_flight = false; events += 'D'; },
          [&] { Check(!gpu_in_flight, "cached pipeline cannot be destroyed in flight");
                cached_objects = 1; events += 'E'; },
          [&] { Check(!gpu_in_flight && cached_objects == 1 && failed_objects == 0,
                      "retry starts after reclamation");
                ++attempts; events += 'R'; return retried; });
      Check(result == retried && attempts == 1 && events == "FDER",
            "OOM recovery is ordered and retries exactly once, even after a repeated failure");
    }
  }
  for (int32_t result : {0, -3, -4, -13, -1000012000}) {
    bool touched = false;
    const auto actual = RecoverPipelineAllocation(result,
        [&] { touched = true; }, [&] { touched = true; }, [&] { touched = true; },
        [&] { touched = true; return int32_t{0}; });
    Check(actual == result && !touched,
          "success/device loss/invalid shader must not trigger memory recovery");
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

void TestGtPressureReplay() {
    // Exact usage/budget/available samples around the confirmed LOW_MEMORY exit.
    struct Sample { uint64_t usage, budget, available; };
    const Sample samples[] = {
        {2278,2992,793}, {2002,2765,847}, {1907,2721,904},
        {2352,2675,357}, {2294,2509,239}, {2484,2659,194},
        {2761,2839,86}, {3559,3684,138}, {5785,5950,183},
    };
    auto previous = 3584 * MiB;
    for (auto sample : samples) {
        const auto proposed = Effective(3584 * MiB, sample.usage * MiB,
            sample.budget * MiB, sample.available * MiB);
        const auto actual = StabilizeGtBudget(3584 * MiB, previous, proposed,
            sample.usage * MiB, sample.budget * MiB, sample.available * MiB);
        Check(actual <= previous, "GT budget cannot grow during the recorded pressure burst");
        Check(actual >= kRuntimeFloor, "GT pressure replay keeps the configured runtime floor");
        previous = actual;
    }
    const auto recovering = StabilizeGtBudget(3584 * MiB, previous, 3584 * MiB,
        2 * GiB, 6 * GiB, 3 * GiB);
    Check(recovering == previous + 128 * MiB, "GT budget recovers gradually after real headroom returns");
    Check(StabilizeGtBudget(0, 0, 0, 0, 0, 0) == 0, "disabled GT budget stays disabled");
}

void TestNeptuniaPressureReplay() {
    struct Sample { uint64_t usage, driver_budget, available; };
    // The exact recorded transition 3584 -> 1547 -> 3584 -> 1554 -> 3584
    // is not a recovery: MemAvailable remained below 1 GiB.
    const Sample samples[] = {
        {1635, 2315, 755}, {1524, 2314, 877}, {1588, 2322, 814},
        {1524, 2363, 932}, {1588, 2314, 805}, {1460, 2291, 922},
        {1909, 2509, 666},
    };
    auto previous = 3584 * MiB;
    bool shrunk = false;
    for (auto sample : samples) {
        const auto proposed = Effective(3584 * MiB, sample.usage * MiB,
                                        sample.driver_budget * MiB, sample.available * MiB);
        const auto stable = StabilizeGtBudget(3584 * MiB, previous, proposed,
                                              sample.usage * MiB,
                                              sample.driver_budget * MiB, sample.available * MiB);
        Check(stable <= previous, "Neptunia must not restore budget under pressure");
        Check(stable >= kRuntimeFloor, "Neptunia respects runtime floor");
        shrunk |= stable < previous;
        previous = stable;
    }
    Check(shrunk, "Neptunia budget actually shrinks");
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

void TestGtPreallocationReclaim() {
	Check(!ShouldPreemptGtImageAllocation(std::nullopt, 5 * GiB, 0, 0),
	      "unknown Android RAM must not enable preallocation eviction");
	Check(!ShouldPreemptGtImageAllocation(2 * GiB, 5 * GiB, 0, 0),
	      "healthy RAM leaves allocation path unchanged");
	Check(!ShouldPreemptGtImageAllocation(512 * MiB, 512 * MiB, 0, 0),
	      "low usage avoids unnecessary recovery");
	Check(ShouldPreemptGtImageAllocation(1200 * MiB, 2500 * MiB, 0, 0),
	      "preempt before system enters the 100 MiB danger zone");
	Check(!ShouldPreemptGtImageAllocation(1200 * MiB, 2500 * MiB,
	                                    2490 * MiB, 0),
	      "no duplicate reclaim on adjacent small allocations");
	Check(ShouldPreemptGtImageAllocation(100 * MiB, 2600 * MiB,
	                                   2500 * MiB, 0),
	      "rapid growth triggers a second reclaim in the same frame");
	Check(ShouldPreemptGtImageAllocation(512 * MiB, 2500 * MiB,
	                                   2500 * MiB, 3),
	      "new frame allows retry on newly cold images");
	Check(!ShouldPreemptGtImageAllocation(512 * MiB, 2500 * MiB,
	                                    2500 * MiB, 2),
	      "short frame gap throttles GPU synchronizations");
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

void TestWrittenDownloadLimits() {
  Check(WrittenDownloadLimit(false, 64 * MiB) == 32 * MiB,
        "normal pressure copies at most 32 MiB");
  Check(WrittenDownloadLimit(true, 64 * MiB) == 64 * MiB,
        "critical pass can handle a 64 MiB image");
  Check(WrittenDownloadLimit(true, 16 * MiB) == 16 * MiB,
        "copy must fit actual staging capacity");
  Check(WrittenDownloadLimit(true, 0) == 0,
        "missing capacity cannot accept a copy");
  Check(WrittenDownloadLimit(true, UINT64_MAX) == 64 * MiB,
        "large buffers never unbound a pass");
  Check(WrittenIdleFrames(true) > 1 &&
            WrittenIdleFrames(false) > WrittenIdleFrames(true),
        "current/recent frames stay protected and normal pressure retains "
        "images longer");
}

} // namespace

int main() {
  Check(ReadbackRows(8192, 128, 32768, 32768, 8 * MiB) == 256,
        "bounded large image window");
  Check(ReadbackRows(129, 128, 32768, 32768, 8 * MiB) == 129,
        "partial final block row");
  Check(ReadbackRows(8192, 128, 65536, 32768, 8 * MiB) == 128,
        "guest padding limit");
  Check(ReadbackRows(8192, 128, 32768, 65536, 8 * MiB) == 128,
        "linear scratch limit");
  Check(ReadbackRows(8192, 128, 32768, 32768, 4 * MiB - 1) == 0,
        "cannot split tile block row");
  Check(ReadbackRows(1, 0, 1, 1, UINT64_MAX) == 0 &&
            ReadbackRows(1, 1, 0, 1, UINT64_MAX) == 0,
        "reject invalid geometry");
  TestPipelineAllocationRecovery();
  TestAutoBudget();
  TestThresholds();
  TestEffective();
  TestGtPressureReplay();
  TestNeptuniaPressureReplay();
  TestUnifiedSystemPressure();
  TestGtPreallocationReclaim();
  TestProcessMemoryReadings();
  TestWrittenDownloadLimits();
  std::printf("GpuMemoryBudgetTests: ok\n");
  return 0;
}
