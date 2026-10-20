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
// robust_buffer_loads, lane_exec_branches, wqm_entry_lane_fold, wave_slices, uniform constant
// buffers). The first two default to on for Mesa Turnip only, the experimental others to off.
// Environment overrides: KYTY_FAST_FMINMAX, KYTY_ROBUST_LOADS, KYTY_LANE_BRANCHES,
// KYTY_WQM_LANE_FOLD, KYTY_WAVE_SLICES, KYTY_UBO_CONSTANTS, KYTY_QUAD_DPP (1 on, 0 off).
// Logs the result.
struct ShaderFastPaths {
	// KYTY_FTZ32 (default on with Mesa Turnip, =0 opts out): ShaderDeviceFeatures::ftz32_ops from the device check (ftzProbe.h).
	uint32_t ftz32_ops        = 0;
	// KYTY_PS_LIVE_ELECT (default on with Mesa Turnip, =0 opts out): ShaderDeviceFeatures::pixel_live_election.
	bool pixel_live_election  = false;
	bool native_float_min_max = false;
	bool robust_buffer_loads  = false;
	bool lane_exec_branches   = false;
	// laneBranchDefaults.h: lane_exec_branches for the listed guest shaders (Mesa Turnip, while
	// KYTY_LANE_BRANCHES is unset; KYTY_LANE_BRANCHES_DEFAULT=0 opts out).
	bool lane_branch_defaults = false;
	bool wqm_entry_lane_fold  = false;
	bool wave_slices          = false;
	// ShaderDeviceFeatures::subgroup_quad_dpp (KYTY_QUAD_DPP; default Mesa Turnip, =0 opts out).
	bool quad_dpp             = false;
	// ShaderDeviceFeatures::bvh_vector_loads (KYTY_BVH_VEC, default off).
	bool bvh_vector_loads = false;
	// ShaderDeviceFeatures::packed_varying_locations (KYTY_PACKED_INTERP; default Mesa Turnip).
	bool packed_interp = false;
	// ShaderDeviceFeatures::drop_carried_vertex_data (KYTY_DROP_CARRIED; default Mesa Turnip).
	bool drop_carried = false;
	// ShaderDeviceFeatures::fold_same_condition_selects (KYTY_SELECT_FOLD; default Mesa Turnip).
	bool select_fold = false;
	// ShaderDeviceFeatures::vs_live_vertex_exec (KYTY_VS_LIVE_EXEC; default Mesa Turnip, =0 opts out).
	bool vs_live_exec = false;
	// ShaderDeviceFeatures::dead_lane_fold (KYTY_DEAD_LANE_FOLD; default off).
	bool dead_lane = false;
	// ShaderDeviceFeatures::single_mode_alias_slots (KYTY_ALIAS_SLOTS; default Mesa Turnip).
	bool alias_slots = false;
	// ShaderDeviceFeatures::constant_lane_broadcast (KYTY_LANE_BROADCAST).
	bool lane_broadcast = false;
	// ShaderDeviceFeatures::first_lane_value_sets (KYTY_FIRSTLANE_SETS).
	bool first_lane_sets = false;
	// ShaderDeviceFeatures::bvh_emulation (KYTY_BVH_OPT=<bit mask>, ShaderBvhEmulation).
	uint32_t bvh_emulation = 0;
	// ShaderDeviceFeatures::uniform_constant_buffer_bytes / uniform_buffers_per_stage
	// (KYTY_UBO_CONSTANTS; default Mesa Turnip, =0 opts out, =cs compute only; needs robustBufferAccess2
	// for uniform buffers). 0: off.
	uint32_t uniform_constant_buffer_bytes = 0;
	uint32_t uniform_buffers_per_stage     = 0;
	// KYTY_UBO_CONSTANTS=cs: compute programs only; the other stages keep storage buffers.
	bool uniform_constants_compute_only = false;
};
[[nodiscard]] ShaderFastPaths ResolveShaderFastPaths(GraphicContext& graphics);

// Per-shader bisecting of the lane fast paths: <NAME>_ONLY=<hex prefixes> keeps the path only for
// matching guest hashes, <NAME>_SKIP=<hex prefixes> drops it for them (KYTY_PIPELINE_STATS
// syntax; NAME is KYTY_LANE_BRANCHES, KYTY_WQM_LANE_FOLD or KYTY_WAVE_SLICES). Both unset: every
// shader.
enum class LaneFastPath { LaneBranches, WqmLaneFold, WaveSlices };
[[nodiscard]] bool LaneFastPathAllowed(LaneFastPath path, uint64_t guest_hash);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_ */
