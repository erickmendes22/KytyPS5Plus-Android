#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"
#include "graphics/host_gpu/renderer/pipeline/fanAutoPolicy.h"
#include "graphics/shader/recompiler/ir/passes/WalkCost.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>

#include <cstdlib>
#include <cstring>

// The knobs of the walk fan-out (walkFan.h); kept apart so the tests link walkFan.cpp alone.
namespace Libs::Graphics::WalkFan {

namespace {

enum class FanMode { Off, On, Auto };

FanMode EnvMode() {
	static const FanMode mode = [] {
		// Default on for Android (Mesa Turnip, the validated target); KYTY_GPU_FAN=0 opts out.
#if defined(__ANDROID__)
		constexpr FanMode default_mode = FanMode::On;
#else
		constexpr FanMode default_mode = FanMode::Off;
#endif
		const char* value    = std::getenv("KYTY_GPU_FAN");
		const char* auto_env = std::getenv("KYTY_FAN_AUTO");
		FanMode     result   = default_mode;
		if (value != nullptr) {
			result = std::strcmp(value, "auto") == 0 ? FanMode::Auto
			         : std::strcmp(value, "1") == 0  ? FanMode::On
			                                         : FanMode::Off;
		}
		if (auto_env != nullptr && std::strcmp(auto_env, "1") == 0) {
			result = FanMode::Auto;
		}
		LOGF("KYTY_GPU_FAN: SRT walk fan-out %s\n",
		     result == FanMode::Auto ? "auto (by walk cost)" : result == FanMode::On ? "on" : "off");
		return result;
	}();
	return mode;
}

// KYTY_GPU_FAN=auto: the front's policy state (Enabled() runs on the front only).
bool AutoEngaged() {
	namespace Cost = ShaderRecompiler::IR::WalkCost;
	static FanAutoPolicy policy = [] {
		FanAutoConfig config;
		if (const char* v = std::getenv("KYTY_FAN_AUTO_ON_US")) config.engage_us = std::atof(v);
		if (const char* v = std::getenv("KYTY_FAN_AUTO_OFF_US")) config.disengage_us = std::atof(v);
		Cost::g_enabled.store(true, std::memory_order_relaxed);
		LOGF("[fan-auto] engage >= %.1f us/walk, disengage <= %.1f us/walk, window %.1f s, dwell %.1f s\n",
		     config.engage_us, config.disengage_us, static_cast<double>(config.window_ns) * 1e-9,
		     static_cast<double>(config.dwell_ns) * 1e-9);
		return FanAutoPolicy(config, true);
	}();
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	const auto decision = policy.Update(now, Cost::g_walk_ns.load(std::memory_order_relaxed),
	                                    Cost::g_walks.load(std::memory_order_relaxed));
	if (decision.evaluated) {
		LOGF("[fan-auto] %s cost=%.1f us/walk walks=%" PRIu64 " fan+pipeline %s\n",
		     decision.switched ? "SWITCH" : "window", decision.cost_us, decision.walks,
		     decision.engaged ? "on" : "off");
		if (decision.switched) {
			std::fprintf(stderr, "[fan-auto] switch: fan+pipeline %s at %.1f us/walk\n",
			             decision.engaged ? "on" : "off", decision.cost_us);
		}
	}
	return decision.engaged;
}

} // namespace

bool Enabled() {
	const FanMode mode = EnvMode();
	// The live key gpu_fan=0/1 overrides every mode (A/B runs).
	const bool fallback = mode == FanMode::Auto ? AutoEngaged() : mode == FanMode::On;
	return LiveKnobs::Int("gpu_fan", fallback ? 1 : 0) != 0;
}

bool AutoMode() {
	return EnvMode() == FanMode::Auto;
}

bool MemoEnabled() {
	return LiveKnobs::Int("fan_memo", 0) != 0;
}

bool Shadow() {
	return LiveKnobs::Int("fan_shadow", 0) != 0;
}

void PollWorkers() {
	const auto workers = LiveKnobs::Int("fan_workers", 0);
	if (workers > 0) {
		Pool().Resize(static_cast<uint32_t>(workers));
	}
}

} // namespace Libs::Graphics::WalkFan
