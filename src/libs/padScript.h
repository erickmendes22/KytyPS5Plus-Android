#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_PADSCRIPT_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_PADSCRIPT_H_

#include "libs/controller.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Timed pad input for unattended runs (KYTY_LIVE_KNOBS pad=...): on-screen touch controls cannot
// hold a stick and press a button at the same time through `adb shell input`.
//
//   <step>;<step>;...      step = <ms>:<left x>,<left y>[,<button>+<button>...][,R=<x>/<y>]
//
// Each step holds the left stick (0-255, 128 = centre, y 0 = up), the right stick (R=, default
// centred) and the named buttons for <ms>, e.g. "3000:128,0;400:128,0,cross;2000:128,0" walks
// forward, jumps, keeps walking; "1500:128,128,R=255/128" turns the camera right.
// Buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options touchpad.
namespace Libs::Controller::PadScript {

struct Step {
	int      ms      = 0;
	int      left_x  = 128;
	int      left_y  = 128;
	uint32_t buttons = 0;
	int      right_x = 128;
	int      right_y = 128;
};

constexpr size_t MAX_STEPS    = 64;
constexpr int    MAX_STEP_MS  = 30000;
constexpr int    MAX_TOTAL_MS = 120000;

namespace Detail {

inline std::string_view Trim(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
		text.remove_suffix(1);
	}
	return text;
}

inline std::optional<int> ParseInt(std::string_view text, int min, int max) {
	text      = Trim(text);
	int value = 0;
	const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (text.empty() || error != std::errc() || ptr != text.data() + text.size() || value < min ||
	    value > max) {
		return std::nullopt;
	}
	return value;
}

inline std::optional<uint32_t> ParseButton(std::string_view name) {
	static constexpr std::array<std::pair<std::string_view, uint32_t>, 16> kButtons {{
	    {"cross", PAD_BUTTON_CROSS},
	    {"circle", PAD_BUTTON_CIRCLE},
	    {"square", PAD_BUTTON_SQUARE},
	    {"triangle", PAD_BUTTON_TRIANGLE},
	    {"up", PAD_BUTTON_UP},
	    {"down", PAD_BUTTON_DOWN},
	    {"left", PAD_BUTTON_LEFT},
	    {"right", PAD_BUTTON_RIGHT},
	    {"l1", PAD_BUTTON_L1},
	    {"r1", PAD_BUTTON_R1},
	    {"l2", PAD_BUTTON_L2},
	    {"r2", PAD_BUTTON_R2},
	    {"l3", PAD_BUTTON_L3},
	    {"r3", PAD_BUTTON_R3},
	    {"options", PAD_BUTTON_OPTIONS},
	    {"touchpad", PAD_BUTTON_TOUCH_PAD},
	}};
	name = Trim(name);
	for (const auto& [button_name, bit]: kButtons) {
		if (name == button_name) {
			return bit;
		}
	}
	return std::nullopt;
}

inline std::optional<uint32_t> ParseButtons(std::string_view text) {
	uint32_t buttons = 0;
	text             = Trim(text);
	while (!text.empty()) {
		const auto end    = std::min(text.find('+'), text.size());
		const auto button = ParseButton(text.substr(0, end));
		if (!button) {
			return std::nullopt;
		}
		buttons |= *button;
		text.remove_prefix(std::min(end + 1, text.size()));
	}
	return buttons;
}

inline std::optional<Step> ParseStep(std::string_view text) {
	const auto colon = text.find(':');
	if (colon == std::string_view::npos) {
		return std::nullopt;
	}
	const auto ms   = ParseInt(text.substr(0, colon), 1, MAX_STEP_MS);
	auto       rest = text.substr(colon + 1);
	const auto c1   = rest.find(',');
	if (!ms || c1 == std::string_view::npos) {
		return std::nullopt;
	}
	const auto x = ParseInt(rest.substr(0, c1), 0, 255);
	rest.remove_prefix(c1 + 1);
	const auto c2 = rest.find(',');
	const auto y  = ParseInt(rest.substr(0, std::min(c2, rest.size())), 0, 255);
	if (!x || !y) {
		return std::nullopt;
	}
	Step step {*ms, *x, *y, 0};
	// Optional fields: buttons (one field, '+' separated, may be empty) and R=<x>/<y>.
	rest = c2 == std::string_view::npos ? std::string_view {} : rest.substr(c2 + 1);
	bool has_rest = c2 != std::string_view::npos;
	while (has_rest) {
		const auto end   = rest.find(',');
		const auto field = Trim(rest.substr(0, std::min(end, rest.size())));
		has_rest         = end != std::string_view::npos;
		rest             = has_rest ? rest.substr(end + 1) : std::string_view {};
		if (field.size() >= 2 && field[0] == 'R' && field[1] == '=') {
			const auto slash = field.find('/');
			if (slash == std::string_view::npos) {
				return std::nullopt;
			}
			const auto rx = ParseInt(field.substr(2, slash - 2), 0, 255);
			const auto ry = ParseInt(field.substr(slash + 1), 0, 255);
			if (!rx || !ry) {
				return std::nullopt;
			}
			step.right_x = *rx;
			step.right_y = *ry;
			continue;
		}
		const auto buttons = ParseButtons(field);
		if (!buttons) {
			return std::nullopt;
		}
		step.buttons |= *buttons;
	}
	return step;
}

} // namespace Detail

// The parsed steps, or nullopt when any step is malformed, there are none or too many, or the
// script runs longer than MAX_TOTAL_MS.
inline std::optional<std::vector<Step>> Parse(std::string_view text) {
	std::vector<Step> steps;
	int               total_ms = 0;
	while (!text.empty()) {
		const auto end  = std::min(text.find(';'), text.size());
		const auto part = Detail::Trim(text.substr(0, end));
		text.remove_prefix(std::min(end + 1, text.size()));
		if (part.empty()) {
			continue;
		}
		const auto step = Detail::ParseStep(part);
		if (!step || steps.size() >= MAX_STEPS) {
			return std::nullopt;
		}
		total_ms += step->ms;
		if (total_ms > MAX_TOTAL_MS) {
			return std::nullopt;
		}
		steps.push_back(*step);
	}
	if (steps.empty()) {
		return std::nullopt;
	}
	return steps;
}

} // namespace Libs::Controller::PadScript

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_PADSCRIPT_H_ */
