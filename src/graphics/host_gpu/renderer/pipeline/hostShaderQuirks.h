#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/deviceQuirks.h"

namespace Libs::Graphics {

[[nodiscard]] ShaderDriverInfo QueryShaderDriverInfo(vk::PhysicalDevice physical_device);

// Quirks detected for the device, then the KYTY_SHADER_QUIRKS overrides ("name,-name,none").
// Logs the result.
[[nodiscard]] ShaderDeviceQuirks ResolveShaderDeviceQuirks(vk::PhysicalDevice physical_device);

struct GraphicContext;

// Emitter fast paths that depend on the host driver (ShaderDeviceFeatures::native_float_min_max,
// robust_buffer_loads, lane_exec_branches, wqm_entry_lane_fold, wave_slices). The first two
// default to on for Mesa Turnip only, the experimental others to off. Environment overrides:
// KYTY_FAST_FMINMAX, KYTY_ROBUST_LOADS, KYTY_LANE_BRANCHES, KYTY_WQM_LANE_FOLD, KYTY_WAVE_SLICES
// (1 on, 0 off).
// Logs the result.
struct ShaderFastPaths {
	bool native_float_min_max = false;
	bool robust_buffer_loads  = false;
	bool lane_exec_branches   = false;
	bool wqm_entry_lane_fold  = false;
	bool wave_slices          = false;
};
[[nodiscard]] ShaderFastPaths ResolveShaderFastPaths(const GraphicContext& graphics);

// Per-shader bisecting of the lane fast paths: <NAME>_ONLY=<hex prefixes> keeps the path only for
// matching guest hashes, <NAME>_SKIP=<hex prefixes> drops it for them (KYTY_PIPELINE_STATS
// syntax; NAME is KYTY_LANE_BRANCHES, KYTY_WQM_LANE_FOLD or KYTY_WAVE_SLICES). Both unset: every
// shader.
enum class LaneFastPath { LaneBranches, WqmLaneFold, WaveSlices };
[[nodiscard]] bool LaneFastPathAllowed(LaneFastPath path, uint64_t guest_hash);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_ */
