#include "graphics/host_gpu/renderer/pipeline/hostShaderQuirks.h"
#include "graphics/host_gpu/renderer/pipeline/knobDefaults.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/ftzProbe.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace Libs::Graphics {

ShaderDriverInfo QueryShaderDriverInfo(vk::PhysicalDevice physical_device) {
	vk::PhysicalDeviceDriverProperties driver {};
	vk::PhysicalDeviceProperties2      properties {};
	properties.pNext = &driver;
	physical_device.getProperties2(&properties);
	return {.vendor_id      = properties.properties.vendorID,
	        .device_id      = properties.properties.deviceID,
	        .driver_id      = static_cast<uint32_t>(driver.driverID),
	        .driver_version = properties.properties.driverVersion};
}

ShaderDeviceQuirks ResolveShaderDeviceQuirks(vk::PhysicalDevice physical_device) {
	auto quirks = DetectShaderDeviceQuirks(QueryShaderDriverInfo(physical_device));
	if (const char* spec = std::getenv("KYTY_SHADER_QUIRKS"); spec != nullptr) {
		std::string error;
		if (!ApplyShaderQuirkOverrides(spec, quirks, &error)) {
			Log::WriteToConsoleAndLog(fmt::format("WARNING: KYTY_SHADER_QUIRKS: {}\n", error));
		}
	}
	Log::WriteToConsoleAndLog(fmt::format("Shader device quirks: {}\n", DescribeShaderQuirks(quirks)));
	return quirks;
}

namespace {

uint32_t EnvBits(const char* name, uint32_t default_value) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return default_value;
	}
	return static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
}

bool EnvToggle(const char* name, bool default_value) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return default_value;
	}
	return !(value[0] == '0' && value[1] == '\0');
}

} // namespace

ShaderFastPaths ResolveShaderFastPaths(GraphicContext& graphics) {
	const bool      turnip = graphics.IsMesaTurnip();
	ShaderFastPaths paths {
	    .native_float_min_max = EnvToggle("KYTY_FAST_FMINMAX", turnip),
	    .robust_buffer_loads  = EnvToggle("KYTY_ROBUST_LOADS", turnip) &&
	                           graphics.optional_features.robust_buffer_access2_dword,
	    .lane_exec_branches   = EnvToggle("KYTY_LANE_BRANCHES", false),
	    .lane_branch_defaults = turnip && std::getenv("KYTY_LANE_BRANCHES") == nullptr &&
	                            EnvToggle("KYTY_LANE_BRANCHES_DEFAULT", true),
	    .wqm_entry_lane_fold  = EnvToggle("KYTY_WQM_LANE_FOLD", false),
	    .wave_slices          = EnvToggle("KYTY_WAVE_SLICES", false),
	    .quad_dpp             = EnvToggle("KYTY_QUAD_DPP", turnip) &&
	                graphics.SubgroupOperationSupported(vk::SubgroupFeatureFlagBits::eQuad),
	    .relative_swizzle     = KnobDefaults::Toggle(std::getenv("KYTY_SWIZZLE_RELATIVE"), turnip) &&
	                        graphics.SubgroupOperationSupported(vk::SubgroupFeatureFlagBits::eShuffleRelative) &&
	                        graphics.SubgroupOperationSupported(vk::SubgroupFeatureFlagBits::eShuffle),
	    .bvh_vector_loads     = EnvToggle("KYTY_BVH_VEC", false),
	    .packed_interp        = EnvToggle("KYTY_PACKED_INTERP", turnip) &&
	                     !graphics.optional_features.fragment_shader_barycentric,
	    .drop_carried         = EnvToggle("KYTY_DROP_CARRIED", turnip),
	    .select_fold          = EnvToggle("KYTY_SELECT_FOLD", turnip),
	    .vs_live_exec         = EnvToggle("KYTY_VS_LIVE_EXEC", turnip),
	    .dead_lane            = EnvToggle("KYTY_DEAD_LANE_FOLD", false),
	    .dead_lane_defaults   = turnip && std::getenv("KYTY_DEAD_LANE_FOLD") == nullptr &&
	                          EnvToggle("KYTY_DEAD_LANE_FOLD_DEFAULT", true),
	    .alias_slots          = EnvToggle("KYTY_ALIAS_SLOTS", turnip),
	    .lane_broadcast       = EnvToggle("KYTY_LANE_BROADCAST", turnip),
	    .first_lane_sets      = EnvToggle("KYTY_FIRSTLANE_SETS", false),
	    .bvh_emulation        = EnvBits("KYTY_BVH_OPT", 0u),
	};
	if (EnvToggle("KYTY_UBO_CONSTANTS", turnip) && graphics.optional_features.robust_uniform_buffer_access2) {
		const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
		const auto  range  = std::min<uint64_t>(limits.maxUniformBufferRange,
		                                        uint64_t {ShaderRecompiler::IR::UniformBufferVec4Count} * 16u);
		const auto  align  = std::max<uint64_t>(limits.minUniformBufferOffsetAlignment, 1u);
		// The binding starts up to align - 16 bytes before a 16-byte aligned guest base.
		if (align <= 256u && range > align) {
			paths.uniform_constant_buffer_bytes = static_cast<uint32_t>(range - (align - 1u));
			paths.uniform_buffers_per_stage =
			    std::min<uint32_t>(limits.maxPerStageDescriptorUniformBuffers / 4u, 8u);
			const char* stages = std::getenv("KYTY_UBO_CONSTANTS");
			paths.uniform_constants_compute_only = stages != nullptr && std::strcmp(stages, "cs") == 0;
		}
	}
	Log::WriteToConsoleAndLog(fmt::format("Shader fast paths: native_float_min_max={} "
	                                      "robust_buffer_loads={} lane_exec_branches={} lane_branch_defaults={} "
	                                      "wqm_entry_lane_fold={} wave_slices={} quad_dpp={} "
	                                      "bvh_vec={} packed_interp={} drop_carried={} select_fold={} vs_live_exec={} dead_lane={} dead_lane_defaults={} alias_slots={} lane_broadcast={} first_lane_sets={} bvh_opt={} "
	                                      "ubo_constants={}B x{}{}\n",
	                                      paths.native_float_min_max, paths.robust_buffer_loads,
	                                      paths.lane_exec_branches, paths.lane_branch_defaults, paths.wqm_entry_lane_fold,
	                                      paths.wave_slices, paths.quad_dpp, paths.bvh_vector_loads, paths.packed_interp, paths.drop_carried, paths.select_fold, paths.vs_live_exec, paths.dead_lane, paths.dead_lane_defaults, paths.alias_slots,
	                                      paths.lane_broadcast, paths.first_lane_sets, paths.bvh_emulation,
	                                      paths.uniform_constant_buffer_bytes,
	                                      paths.uniform_buffers_per_stage,
	                                      paths.uniform_constants_compute_only ? " (compute only)" : ""));
	paths.fold_user_depth = KnobDefaults::FoldUserDepth(std::getenv("KYTY_FOLD_USER_DEPTH"), turnip);
	Log::WriteToConsoleAndLog(fmt::format("Shader fast paths: relative_swizzle={} fold_user_depth={}\n",
	                                      paths.relative_swizzle, paths.fold_user_depth));
	paths.ftz32_ops = ProbeFtz32Ops(graphics, turnip);
	paths.pixel_live_election = EnvToggle("KYTY_PS_LIVE_ELECT", turnip);
	if (paths.pixel_live_election) {
		Log::WriteToConsoleAndLog("KYTY_PS_LIVE_ELECT: pixel-shader append counters elected among non-helper lanes\n");
	}
	return paths;
}

namespace {

struct HashFilter {
	std::vector<std::string> only;
	std::vector<std::string> skip;
};

HashFilter ReadHashFilter(const char* name) {
	HashFilter filter;
	const auto read = [](const std::string& variable) {
		const char* spec = std::getenv(variable.c_str());
		return spec != nullptr ? PipelineStats::ParseSpec(spec) : std::vector<std::string> {};
	};
	filter.only = read(std::string(name) + "_ONLY");
	filter.skip = read(std::string(name) + "_SKIP");
	if (!filter.only.empty() || !filter.skip.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("{}: only={} skip={} prefixes\n", name,
		                                      filter.only.size(), filter.skip.size()));
	}
	return filter;
}

} // namespace

bool LaneFastPathAllowed(LaneFastPath path, uint64_t guest_hash) {
	static const HashFilter lane_branches = ReadHashFilter("KYTY_LANE_BRANCHES");
	static const HashFilter wqm_fold      = ReadHashFilter("KYTY_WQM_LANE_FOLD");
	static const HashFilter wave_slices   = ReadHashFilter("KYTY_WAVE_SLICES");
	const auto& filter = path == LaneFastPath::LaneBranches ? lane_branches
	                   : path == LaneFastPath::WqmLaneFold  ? wqm_fold
	                                                        : wave_slices;
	if (!filter.only.empty() && !PipelineStats::Matches(filter.only, guest_hash)) return false;
	return filter.skip.empty() || !PipelineStats::Matches(filter.skip, guest_hash);
}

} // namespace Libs::Graphics
