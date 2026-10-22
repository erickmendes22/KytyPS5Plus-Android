#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_KNOBDEFAULTS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_KNOBDEFAULTS_H_

#include <cstdint>
#include <cstdlib>

// Default resolution for shader knobs whose default depends on the driver (hostShaderQuirks.cpp).
// Pure functions of the environment value, unit-tested in tests/KnobDefaultsTests.cpp.
namespace Libs::Graphics::KnobDefaults {

// KYTY_FOLD_USER_DEPTH default with Mesa Turnip: constant propagation requeues consumers up to
// 16 levels after a change. The same SPIR-V as the full cone for every shader dumped from UE5
// PPSA14708 (175) and Astro Bot (116), and UE5's cs b48e83a9 translates in 0.3 s instead of 27 s.
inline constexpr uint32_t kFoldUserDepthTurnip = 16;

// A boolean knob: unset or empty -> default_value, "0" -> off, anything else -> on.
[[nodiscard]] inline bool Toggle(const char* value, bool default_value) {
	if (value == nullptr || *value == '\0') {
		return default_value;
	}
	return !(value[0] == '0' && value[1] == '\0');
}

// KYTY_FOLD_USER_DEPTH: unset or empty -> 16 with Turnip, 0 (the whole consumer cone) elsewhere;
// an explicit value wins (0 = the whole cone).
[[nodiscard]] inline uint32_t FoldUserDepth(const char* value, bool turnip) {
	if (value == nullptr || *value == '\0') {
		return turnip ? kFoldUserDepthTurnip : 0u;
	}
	return static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
}

} // namespace Libs::Graphics::KnobDefaults

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_KNOBDEFAULTS_H_ */
