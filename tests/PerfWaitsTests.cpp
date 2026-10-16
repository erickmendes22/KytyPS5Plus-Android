#include "common/perfWaits.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace Common::PerfStats;

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", what);
		g_failures++;
	}
}

bool Contains(const std::string& text, const char* part) {
	return text.find(part) != std::string::npos;
}

void TestDisabledCountsNothing() {
	Detail::g_enabled.store(false);
	AddWait(Wait::QueueSubmit, 5'000'000);
	Count(Counter::Submits, 7);
	Detail::g_enabled.store(true);
	const auto line = TakeWaitSummary(1.0, 1);
	Check(Contains(line, "submits=0 "), "disabled counters stay zero");
	Check(!Contains(line, "submit="), "disabled waits are not reported");
}

void TestPerFrameAndReset() {
	Detail::g_enabled.store(true);
	AddWait(Wait::QueueSubmit, 3'000'000);
	AddWait(Wait::QueueSubmit, 1'000'000);
	AddWait(Wait::GuestSuspendPoint, 10'000'000);
	Count(Counter::Submits, 8);
	Count(Counter::GpuBusyNs, 20'000'000);
	const auto line = TakeWaitSummary(1.0, 2);
	Check(Contains(line, "per frame"), "flips select per-frame units");
	Check(Contains(line, "submits=4 "), "counters divide by flips");
	Check(Contains(line, "gpu_busy=10.0ms"), "GPU busy is per frame");
	Check(Contains(line, "(2% of wall"), "GPU busy share of wall time");
	Check(Contains(line, "gpu-thread: submit=2.0(1)"), "waits are ms and count per frame");
	Check(Contains(line, "suspend=5.0(0)"), "guest waits are reported");
	const auto next = TakeWaitSummary(1.0, 2);
	Check(Contains(next, "submits=0 ") && !Contains(next, "submit="), "taking resets the period");
}

void TestNoFlipsReportsPerSecond() {
	Detail::g_enabled.store(true);
	Count(Counter::Draws, 10);
	const auto line = TakeWaitSummary(2.0, 0);
	Check(Contains(line, "per second"), "no flips selects per-second units");
	Check(Contains(line, "draws=5 "), "per-second counters divide by the period");
}

void TestWaitSiteNesting() {
	Check(CurrentWaitSite() == Wait::SemOther, "default site");
	{
		const ScopedWaitSite outer(Wait::SemReadFault);
		Check(CurrentWaitSite() == Wait::SemReadFault, "outer site");
		{
			const ScopedWaitSite inner(Wait::SemFlush);
			Check(CurrentWaitSite() == Wait::SemFlush, "inner site");
		}
		Check(CurrentWaitSite() == Wait::SemReadFault, "inner site restored");
	}
	Check(CurrentWaitSite() == Wait::SemOther, "outer site restored");
}

void TestScopedWaitTimesOnlyWhenEnabled() {
	Detail::g_enabled.store(true);
	(void)TakeWaitSummary(1.0, 1);
	{
		const ScopedWait wait(Wait::GpuIdle);
	}
	Check(Contains(TakeWaitSummary(1.0, 1), "idle="), "enabled scoped wait is recorded");
	Detail::g_enabled.store(false);
	{
		const ScopedWait wait(Wait::GpuIdle);
	}
	Detail::g_enabled.store(true);
	Check(!Contains(TakeWaitSummary(1.0, 1), "idle="), "disabled scoped wait is not recorded");
}

} // namespace

int main() {
	TestDisabledCountsNothing();
	TestPerFrameAndReset();
	TestNoFlipsReportsPerSecond();
	TestWaitSiteNesting();
	TestScopedWaitTimesOnlyWhenEnabled();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("perf_waits_tests: all checks passed\n");
	return EXIT_SUCCESS;
}
