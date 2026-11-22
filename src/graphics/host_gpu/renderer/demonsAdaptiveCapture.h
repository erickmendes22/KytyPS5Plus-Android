#pragma once

#include <algorithm>
#include <cstdint>

// Shared by the compositor's real HDR observations and the lighting producers.
// Frame numbers only measure intervals AFTER an observed event; no boot or
// cutscene-specific absolute frame number is used.
namespace Libs::Graphics::DemonsAdaptiveCapture {

struct Observation {
	bool triggered = false;
	bool has_bright_reference = false;
	uint32_t low_streak = 0;
	double bright_reference = 0.0;
	const char* reason = "scouting";
};

class Gate {
public:
	void MarkGeometry(int32_t frame, uint32_t colored) {
		// Evidence from the *executed deferred-lighting queue*, not a
		// cutscene frame number or the mere presence of a compositor.
		if (colored != 0 && frame >= 0) m_last_geometry = frame;
	}

	bool ShouldSample(int32_t frame) {
		if (frame < 0) return false;
		if (frame < m_last_sample) Reset();
		return m_last_sample < 0 || frame - m_last_sample >= 24;
	}

	Observation Observe(int32_t frame, double level, uint32_t visible, uint64_t guest) {
		if (frame < m_last_sample) Reset();
		m_last_sample = frame;
		// The scene address/size may change during a level transition; do
		// not compare incompatible buffers as though they were one image.
		if (m_scene_guest && guest != m_scene_guest) {
			m_bright_reference = 0.0;
			m_low_streak = 0;
			m_scene_samples = 0;
		}
		m_scene_guest = guest;
		if (m_last_geometry < 0 || frame - m_last_geometry > 300) {
			// A fade or a black loading screen is not proof of gameplay.
			m_low_streak = 0;
			m_scene_samples = 0;
			m_bright_reference = 0.0;
			return {false, false, 0, 0.0, "no_deferred_geometry"};
		}
		++m_scene_samples;
		const bool lit = level >= 0.02 && visible >= 3;
		if (lit) {
			m_bright_reference = std::max(m_bright_reference * 0.98, level);
			m_low_streak = 0;
		} else {
			const bool dark_relative = m_bright_reference >= 0.02 &&
			    level <= m_bright_reference * 0.10;
			const bool dark_absolute = level < 0.006 && visible <= 2;
			m_low_streak = (dark_relative || dark_absolute)
			    ? m_low_streak + 1 : 0;
		}
		Observation result {false, m_bright_reference >= 0.02, m_low_streak,
		                    m_bright_reference, "scouting"};
		if (m_last_trigger >= 0 && frame - m_last_trigger < 180) {
			result.reason = "cooldown";
			return result;
		}
		// Wait for a sustained change, not one fade/black loading frame.
		const bool transition = m_bright_reference >= 0.02 && m_low_streak >= 3;
		const bool dark_entry = m_bright_reference < 0.02 &&
		    m_scene_samples >= 5 && m_low_streak >= 5;
		if (transition || dark_entry) {
			m_last_trigger = frame;
			m_low_streak = 0;
			result.triggered = true;
			result.reason = transition ? "bright_to_dark" : "dark_scene_entry";
		}
		return result;
	}

	bool CaptureDue(int32_t frame) const {
		if (frame <= m_last_trigger || m_last_trigger < 0) return false;
		const auto elapsed = frame - m_last_trigger;
		// Two opportunities to bind the complete producer->atlas chain.
		// They follow the actual observed dark scene, NOT an assumed frame.
		return (elapsed >= 1 && elapsed <= 12) ||
		       (elapsed >= 24 && elapsed <= 48) ||
		       (elapsed >= 72 && elapsed <= 96);
	}

private:
	void Reset() {
		m_last_sample = -1;
		m_last_trigger = -1;
		m_bright_reference = 0.0;
		m_scene_guest = 0;
		m_last_geometry = -1;
		m_scene_samples = 0;
		m_low_streak = 0;
	}
	int32_t m_last_sample = -1;
	int32_t m_last_trigger = -1;
	double m_bright_reference = 0.0;
	uint64_t m_scene_guest = 0;
	int32_t m_last_geometry = -1;
	uint32_t m_scene_samples = 0;
	uint32_t m_low_streak = 0;
};

inline Gate& Current() {
	static Gate gate;
	return gate;
}

} // namespace Libs::Graphics::DemonsAdaptiveCapture
