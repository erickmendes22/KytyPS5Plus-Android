// WalkFan::FanAutoPolicy (KYTY_GPU_FAN=auto): window means, hysteresis, dwell, too-few-walk windows.

#include "graphics/host_gpu/renderer/pipeline/fanAutoPolicy.h"

#include <cstdint>
#include <cstdio>

namespace {

using Libs::Graphics::WalkFan::FanAutoConfig;
using Libs::Graphics::WalkFan::FanAutoPolicy;

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FanAutoPolicyTests: failed: %s\n", message);
		g_failures++;
	}
}

constexpr int64_t kMs = 1'000'000;

// Drives a policy through `seconds` of 100 ms steps at a fixed walk cost and rate.
struct Feed {
	FanAutoPolicy policy;
	int64_t       now   = 0;
	uint64_t      ns    = 0;
	uint64_t      walks = 0;
	int           switches = 0;

	explicit Feed(bool engaged = true): policy(FanAutoConfig {}, engaged) { policy.Update(now, ns, walks); }

	void Run(double cost_us, uint64_t walks_per_second, int64_t milliseconds) {
		for (int64_t t = 0; t < milliseconds; t += 100) {
			now += 100 * kMs;
			const uint64_t step = walks_per_second / 10;
			walks += step;
			ns += static_cast<uint64_t>(cost_us * 1000.0 * static_cast<double>(step));
			switches += policy.Update(now, ns, walks).switched ? 1 : 0;
		}
	}
};

void TestCheapWalksDisengage() {
	Feed feed;                      // starts engaged (today's default)
	feed.Run(10.0, 25'000, 5'000);  // Delivery Service-like, before the dwell ends
	Check(feed.policy.Engaged(), "T1 no switch before the dwell");
	feed.Run(10.0, 25'000, 4'000);
	Check(!feed.policy.Engaged(), "T1 cheap walks disengage after the dwell");
	Check(feed.switches == 1, "T1 exactly one switch");
	std::puts("PASS T1 cheap walks disengage");
}

void TestExpensiveWalksStayEngaged() {
	Feed feed;
	feed.Run(48.0, 20'000, 60'000); // Astro-like
	Check(feed.policy.Engaged() && feed.switches == 0, "T2 expensive walks stay engaged");
	std::puts("PASS T2 expensive walks stay engaged");
}

void TestHysteresisBand() {
	Feed feed;
	feed.Run(21.0, 20'000, 30'000); // inside (18, 25): no change either way
	Check(feed.policy.Engaged() && feed.switches == 0, "T3 engaged stays inside the band");
	Feed off(false);
	off.Run(21.0, 20'000, 30'000);
	Check(!off.policy.Engaged() && off.switches == 0, "T3 disengaged stays inside the band");
	std::puts("PASS T3 hysteresis band");
}

void TestReengage() {
	Feed feed;
	feed.Run(10.0, 25'000, 10'000);
	Check(!feed.policy.Engaged(), "T4 off on cheap walks");
	feed.Run(40.0, 20'000, 10'000);
	Check(feed.policy.Engaged(), "T4 back on when walks get expensive");
	Check(feed.switches == 2, "T4 two switches");
	std::puts("PASS T4 re-engage");
}

void TestNoFlapping() {
	// Cost alternating across the band every 2 s: at most one switch per dwell (6 s).
	Feed feed;
	for (int i = 0; i < 30; i++) {
		feed.Run(i % 2 == 0 ? 12.0 : 35.0, 20'000, 2'000);
	}
	Check(feed.switches <= 60 / 6 + 1, "T5 switches limited by the dwell");
	std::printf("  T5: %d switches in 60 s\n", feed.switches);
	std::puts("PASS T5 no flapping");
}

void TestTooFewWalks() {
	Feed feed;
	feed.Run(5.0, 100, 30'000); // a menu: 200 walks per window < min_walks
	Check(feed.policy.Engaged() && feed.switches == 0, "T6 windows with too few walks decide nothing");
	std::puts("PASS T6 too few walks");
}

void TestDecisionReport() {
	FanAutoPolicy policy;
	(void)policy.Update(0, 0, 0);
	auto d = policy.Update(1 * kMs, 1000, 1);
	Check(!d.evaluated, "T7 no decision inside a window");
	d = policy.Update(2'000 * kMs, 10'000'000, 1000);
	Check(d.evaluated && d.walks == 1000 && d.cost_us > 9.99 && d.cost_us < 10.01, "T7 window mean cost");
	Check(!d.switched && d.engaged, "T7 no switch before the dwell");
	std::puts("PASS T7 decision report");
}

} // namespace

int main() {
	TestCheapWalksDisengage();
	TestExpensiveWalksStayEngaged();
	TestHysteresisBand();
	TestReengage();
	TestNoFlapping();
	TestTooFewWalks();
	TestDecisionReport();
	if (g_failures != 0) {
		std::fprintf(stderr, "FanAutoPolicyTests: %d failure(s)\n", g_failures);
		return 1;
	}
	std::puts("FanAutoPolicyTests: all cases passed");
	return 0;
}
