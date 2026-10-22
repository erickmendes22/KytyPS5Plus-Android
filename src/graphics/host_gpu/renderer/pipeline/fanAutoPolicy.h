#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_FANAUTOPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_FANAUTOPOLICY_H_

#include <cstdint>

// KYTY_GPU_FAN=auto (walkFan.h): whether the SRT walk fan-out and the two-stage GPU thread pay for
// themselves depends on how expensive the walks are. Astro Bot (~50 us per walk) gains ~15% from
// them; Totally Reliable Delivery Service (~10 us per walk, ~970 draws per frame) loses ~25%, because
// the handoffs and drains cost more than the parallel walks save (research/NEXT.md, g1_ab2/g1_ab4).
//
// This is the decision logic alone (no clocks, no globals) so a host test can drive it. The front
// feeds it the running totals of walk time and walk count; at the end of each window (window_ns
// and at least min_walks) it compares the window's mean walk cost with the thresholds:
//   engaged and cost <= disengage_us  -> disengage
//   disengaged and cost >= engage_us  -> engage
// A switch needs dwell_ns since the previous one, so a scene near a threshold cannot flap. Windows
// with too few walks (menus, loading) make no decision.
namespace Libs::Graphics::WalkFan {

struct FanAutoConfig {
	double   engage_us    = 25.0;
	double   disengage_us = 18.0;
	int64_t  window_ns    = 2'000'000'000;
	uint64_t min_walks    = 500;
	int64_t  dwell_ns     = 6'000'000'000;
};

class FanAutoPolicy {
public:
	struct Decision {
		bool     evaluated = false; // a window closed with enough walks
		bool     switched  = false;
		bool     engaged   = false; // the state after this update
		double   cost_us   = 0.0;   // the closed window's mean walk cost
		uint64_t walks     = 0;     // walks in the closed window
	};

	explicit FanAutoPolicy(FanAutoConfig config = {}, bool engaged = true): m_config(config), m_engaged(engaged) {}

	[[nodiscard]] bool Engaged() const noexcept { return m_engaged; }
	[[nodiscard]] const FanAutoConfig& Config() const noexcept { return m_config; }

	// now_ns: a monotonic clock; total_walk_ns / total_walks: running totals (never decreasing).
	Decision Update(int64_t now_ns, uint64_t total_walk_ns, uint64_t total_walks) noexcept {
		Decision decision;
		decision.engaged = m_engaged;
		if (!m_started) {
			Restart(now_ns, total_walk_ns, total_walks);
			m_last_switch_ns = now_ns;
			m_started        = true;
			return decision;
		}
		if (now_ns - m_window_start_ns < m_config.window_ns) {
			return decision;
		}
		const uint64_t walks = total_walks - m_window_walks;
		const uint64_t ns    = total_walk_ns - m_window_walk_ns;
		Restart(now_ns, total_walk_ns, total_walks);
		if (walks < m_config.min_walks || walks == 0) {
			return decision;
		}
		decision.evaluated = true;
		decision.walks     = walks;
		decision.cost_us   = static_cast<double>(ns) / static_cast<double>(walks) / 1000.0;
		const bool dwell_done = now_ns - m_last_switch_ns >= m_config.dwell_ns;
		const bool flip = m_engaged ? decision.cost_us <= m_config.disengage_us
		                            : decision.cost_us >= m_config.engage_us;
		if (flip && dwell_done) {
			m_engaged          = !m_engaged;
			m_last_switch_ns   = now_ns;
			decision.switched  = true;
			decision.engaged   = m_engaged;
		}
		return decision;
	}

private:
	void Restart(int64_t now_ns, uint64_t total_walk_ns, uint64_t total_walks) noexcept {
		m_window_start_ns = now_ns;
		m_window_walk_ns  = total_walk_ns;
		m_window_walks    = total_walks;
	}

	FanAutoConfig m_config;
	bool          m_engaged;
	bool          m_started         = false;
	int64_t       m_window_start_ns = 0;
	uint64_t      m_window_walk_ns  = 0;
	uint64_t      m_window_walks    = 0;
	int64_t       m_last_switch_ns  = 0;
};

} // namespace Libs::Graphics::WalkFan

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_FANAUTOPOLICY_H_
