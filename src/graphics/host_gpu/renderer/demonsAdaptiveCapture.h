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
			m_dark_latched = false;
			m_recovered_streak = 0;
			m_last_bright_sample = -1;
			m_late_dark_streak = 0;
			m_late_dark_candidate = false;
			m_late_dark_sample = -1;
			m_late_capture_count = 0;
			m_last_late_capture = -1;
		}
		m_scene_guest = guest;
		if (m_last_geometry < 0 || frame - m_last_geometry > 300) {
			// A fade or a black loading screen is not proof of gameplay.
			m_low_streak = 0;
			m_scene_samples = 0;
			m_bright_reference = 0.0;
			m_last_bright_sample = -1;
			return {false, false, 0, 0.0, "no_deferred_geometry"};
		}
		++m_scene_samples;
		const bool lit = level >= 0.02 && visible >= 3;
		if (lit) {
			m_last_bright_sample = frame;
			// A prolonged blackout is one transition, not a new event every
			// 180 frames. Rearm only when actual lighting recovers twice.
			m_recovered_streak = m_dark_latched ? m_recovered_streak + 1 : 0;
			if (m_dark_latched && m_recovered_streak >= 2) {
				m_dark_latched = false;
				m_last_trigger = -1;
				m_last_event_capture = -1;
				m_event_capture_count = 0;
				m_bright_reference = level;
				m_scene_samples = 1;
				m_late_dark_streak = 0;
				m_late_dark_candidate = false;
				m_late_dark_sample = -1;
				m_late_capture_count = 0;
				m_last_late_capture = -1;
			}
			if (!m_dark_latched)
				m_bright_reference = std::max(m_bright_reference * 0.98, level);
			m_low_streak = 0;
		} else {
			m_recovered_streak = 0;
			if (!m_dark_latched) {
				const bool dark_relative = m_bright_reference >= 0.02 &&
				    level <= m_bright_reference * 0.10;
				const bool dark_absolute = level < 0.006 && visible <= 2;
				m_low_streak = (dark_relative || dark_absolute)
				    ? m_low_streak + 1 : 0;
			}
		}
		Observation result {false, m_bright_reference >= 0.02, m_low_streak,
		                    m_bright_reference, "scouting"};
		if (m_dark_latched) {
			// The HDR samples arrive every 24 frames, but verified colored
			// pixels from queue 33 are captured every ~240 frames. Require a
			// *post-fade* geometry observation without pretending a geometry
			// readback happens at every HDR sample. The result remains a
			// candidate, not proof of incorrectly dark gameplay.
			const bool post_fade_geometry = m_last_geometry >= m_last_trigger &&
			    frame >= m_last_geometry && int64_t{frame} - m_last_geometry <= 240;
			// The actual 762 capture stays near-black at frame 527
			// (mean 0.0105114, 3/64 lit pixels), even though its tiny HUD
			// contribution exceeded the old strict 2-pixel threshold.
			// Judge against the observed bright reference as well as an
			// absolute dim ceiling; this arms a capture, NOT a rendering fix.
			const bool still_dim = level < 0.015 && visible <= 4 &&
			    (m_bright_reference < 0.02 || level < m_bright_reference * 0.25);
			if (still_dim) {
				// Track the continuous dark samples independently of the
				// sparsely sampled GPU geometry; only ARM a recording after
				// a new verified queue-33 geometry sample follows the fade.
				++m_late_dark_streak;
				m_late_dark_sample = frame;
			} else {
				m_late_dark_streak = 0;
				m_late_dark_sample = -1;
			}
			m_late_dark_candidate = m_last_trigger >= 0 &&
			    int64_t{frame} - m_last_trigger >= 96 &&
			    m_late_dark_streak >= 4 && post_fade_geometry && still_dim;
			result.reason = m_late_dark_candidate
			    ? "post_transition_dark_candidate" : "sustained_dark";
			return result;
		}
		if (m_last_trigger >= 0 && frame - m_last_trigger < 180) {
			result.reason = "cooldown";
			return result;
		}
		// Wait for a sustained change, not one fade/black loading frame.
		const bool transition = m_bright_reference >= 0.02 && m_low_streak >= 3;
		const bool dark_entry = m_bright_reference < 0.02 &&
		    m_scene_samples >= 5 && m_low_streak >= 5;
		if (transition || dark_entry) {
			m_dark_latched = true;
			m_recovered_streak = 0;
			m_last_trigger = frame;
			m_last_event_capture = -1;
			m_event_capture_count = 0;
			m_low_streak = 0;
			m_late_dark_streak = 0;
			m_late_dark_candidate = false;
			m_late_dark_sample = -1;
			m_last_late_capture = -1;
			m_late_capture_count = 0;
			result.triggered = true;
			result.reason = transition ? "bright_to_dark" : "dark_scene_entry";
		}
		return result;
	}

	bool PostTransitionDarkDue(int32_t frame) const {
		return m_dark_latched && m_late_dark_candidate && m_late_dark_sample >= 0 &&
		       frame >= m_late_dark_sample && frame - m_late_dark_sample <= 24 &&
		       m_last_geometry >= m_last_trigger && frame >= m_last_geometry &&
		       int64_t{frame} - m_last_geometry <= 240;
	}

	bool CaptureDue(int32_t frame) const {
		if (m_last_trigger < 0 || frame <= m_last_trigger) return false;
		if (PostTransitionDarkDue(frame)) {
			if (frame == m_last_late_capture) return true;
			return m_late_capture_count < 3 &&
			       (m_last_late_capture < 0 || frame - m_last_late_capture >= 24);
		}
		// Allow all coefficient writers sharing one frame (the atlas can use
		// several), but bound the heavy readbacks to three successful frames
		// separated by at least 24 *observed* guest frames.
		if (frame == m_last_event_capture) return true;
		return m_event_capture_count < 3 && frame - m_last_trigger <= 144 &&
		       (m_last_event_capture < 0 || frame - m_last_event_capture >= 24);
	}

	bool BrightSceneActive(int32_t frame) const {
		return frame >= m_last_sample && m_last_sample >= 0 &&
		       frame - m_last_sample <= 24 && !m_dark_latched &&
		       m_last_bright_sample == m_last_sample && m_bright_reference >= 0.02;
	}
	bool BrightBaselineDue(int32_t frame) const {
		return BrightSceneActive(frame) && frame > m_last_sample &&
		       frame - m_last_sample <= 24;
	}
	bool DarkOnsetDue(int32_t frame) const {
		// Capture the first loss of light, not only the later adaptive
		// trigger. The reference is from the same observed scene image.
		return !m_dark_latched && m_last_sample >= 0 && frame > m_last_sample &&
		       frame - m_last_sample <= 24 && m_bright_reference >= 0.02 &&
		       m_low_streak == 1;
	}

	bool CapturedFrame(int32_t frame) const {
		// Align the compositor check with the actual coefficient/atlas pair
		// recorded in this guest frame, not with an approximate timer.
		return frame >= 0 &&
		       (frame == m_last_event_capture || frame == m_last_late_capture);
	}

	void NoteCaptured(int32_t frame) {
		if (PostTransitionDarkDue(frame)) {
			if (frame != m_last_late_capture && CaptureDue(frame)) {
				m_last_late_capture = frame;
				++m_late_capture_count;
			}
			return;
		}
		if (frame != m_last_event_capture && CaptureDue(frame)) {
			m_last_event_capture = frame;
			++m_event_capture_count;
		}
	}

private:
	void Reset() {
		m_last_sample = -1;
		m_last_trigger = -1;
		m_last_event_capture = -1;
		m_event_capture_count = 0;
		m_bright_reference = 0.0;
		m_scene_guest = 0;
		m_last_geometry = -1;
		m_scene_samples = 0;
		m_dark_latched = false;
		m_recovered_streak = 0;
		m_low_streak = 0;
		m_last_bright_sample = -1;
		m_late_dark_streak = 0;
		m_late_dark_candidate = false;
		m_late_dark_sample = -1;
		m_late_capture_count = 0;
		m_last_late_capture = -1;
	}
	int32_t m_last_sample = -1;
	int32_t m_last_bright_sample = -1;
	int32_t m_last_trigger = -1;
	int32_t m_last_event_capture = -1;
	uint32_t m_event_capture_count = 0;
	double m_bright_reference = 0.0;
	uint64_t m_scene_guest = 0;
	int32_t m_last_geometry = -1;
	uint32_t m_scene_samples = 0;
	bool m_dark_latched = false;
	uint32_t m_recovered_streak = 0;
	uint32_t m_low_streak = 0;
	uint32_t m_late_dark_streak = 0;
	bool m_late_dark_candidate = false;
	int32_t m_late_dark_sample = -1;
	int32_t m_last_late_capture = -1;
	uint32_t m_late_capture_count = 0;
};

// Separate producer snapshots from the compositors' paired-capture budget.
// A pre-gameplay producer dispatch must NEVER exhaust the dark-phase capture.
class RayProducerCaptureGate {
public:
	void ObserveFrame(int32_t frame) {
		if (m_last_frame >= 0 && frame < m_last_frame) *this = {};
		m_last_frame = frame;
	}
	bool Due(int32_t frame, bool onset, bool late) const {
		if (frame < 0) return false;
		if (late) return m_dark_count < 3 &&
		    (m_last_dark < 0 || (frame >= m_last_dark && frame - m_last_dark >= 24));
		return onset && !m_onset_captured;
	}
	void NoteCaptured(int32_t frame, bool onset, bool late) {
		if (!Due(frame, onset, late)) return;
		if (late) {
			++m_dark_count;
			m_last_dark = frame;
		} else {
			m_onset_captured = true;
		}
	}
private:
	bool m_onset_captured = false;
	uint32_t m_dark_count = 0;
	int32_t m_last_dark = -1;
	int32_t m_last_frame = -1;
};

inline Gate& Current() {
	static Gate gate;
	return gate;
}

} // namespace Libs::Graphics::DemonsAdaptiveCapture
