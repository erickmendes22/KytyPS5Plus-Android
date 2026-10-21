#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DEADLANEDEFAULTS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DEADLANEDEFAULTS_H_

#include <algorithm>
#include <array>
#include <cstdint>

namespace Libs::Graphics {

// Guest compute shaders that get ShaderDeviceFeatures::dead_lane_fold by default with Mesa Turnip
// while KYTY_DEAD_LANE_FOLD itself is unset (its global use is not measured for every shader).
// KYTY_DEAD_LANE_FOLD=0|1 (explicit) or KYTY_DEAD_LANE_FOLD_DEFAULT=0 replace the list; the live
// knob dead_lane=0|1 still overrides per run.
//   6856320a9c6f89fd  Astro Bot RT lighting (cs-indirect): folded=740 narrowed=174; c17_rtC in-run
//                     A/B RT 14.79 -> 12.22 ms/dispatch at the 6.01 step, 16.57 -> 13.82 at 6.81,
//                     pixels at the animation floor. Its tile-class siblings d2c5a665 / f4387ca7 /
//                     a71743d8 fold similarly (685-747 folds) but stay below the top-60 GPU regions,
//                     so their gain is not measured and they are not listed.
inline constexpr std::array<uint64_t, 1> DeadLaneFoldDefaultShaders {0x6856320a9c6f89fdull};

[[nodiscard]] constexpr bool DeadLaneFoldByDefault(uint64_t guest_hash) {
	return std::ranges::find(DeadLaneFoldDefaultShaders, guest_hash) != DeadLaneFoldDefaultShaders.end();
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DEADLANEDEFAULTS_H_
