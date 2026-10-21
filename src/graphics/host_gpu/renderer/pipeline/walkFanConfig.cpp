#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"

#include <cstdlib>
#include <cstring>

// The knobs of the walk fan-out (walkFan.h); kept apart so the tests link walkFan.cpp alone.
namespace Libs::Graphics::WalkFan {

bool Enabled() {
	static const bool env_on = [] {
		// Default on for Android (Mesa Turnip, the validated target); KYTY_GPU_FAN=0 opts out.
#if defined(__ANDROID__)
		constexpr bool default_on = true;
#else
		constexpr bool default_on = false;
#endif
		const char* value = std::getenv("KYTY_GPU_FAN");
		const bool  on    = value == nullptr ? default_on : std::strcmp(value, "1") == 0;
		LOGF("KYTY_GPU_FAN: SRT walk fan-out %s\n", on ? "on" : "off");
		return on;
	}();
	return LiveKnobs::Int("gpu_fan", env_on ? 1 : 0) != 0;
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
