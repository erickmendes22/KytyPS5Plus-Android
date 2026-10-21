#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_LANEBRANCHDEFAULTS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_LANEBRANCHDEFAULTS_H_

#include <algorithm>
#include <array>
#include <cstdint>

namespace Libs::Graphics {

// Guest shaders that get ShaderDeviceFeatures::lane_exec_branches by default with Mesa Turnip while
// KYTY_LANE_BRANCHES itself stays off (its global use is not confirmed for every shader).
// KYTY_LANE_BRANCHES=0|1 (explicit) or KYTY_LANE_BRANCHES_DEFAULT=0 replace the list;
// KYTY_LANE_BRANCHES_SKIP=<prefixes> still drops listed hashes.
//   6856320a9c6f89fd  Astro Bot RT pass (cs-indirect): 19 vote regions converted, ir3 16710 -> 14785
//                     instructions; c16_rtB in-run A/B at full clock RT 12.13 -> 11.31 ms/frame,
//                     pixels at the animation floor.
inline constexpr std::array<uint64_t, 1> LaneBranchDefaultShaders {0x6856320a9c6f89fdull};

[[nodiscard]] constexpr bool LaneBranchesByDefault(uint64_t guest_hash) {
	return std::ranges::find(LaneBranchDefaultShaders, guest_hash) != LaneBranchDefaultShaders.end();
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_LANEBRANCHDEFAULTS_H_
