#include "graphics/host_gpu/renderer/pipeline/hostShaderQuirks.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"

#include <cstdlib>
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

bool EnvToggle(const char* name, bool default_value) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return default_value;
	}
	return !(value[0] == '0' && value[1] == '\0');
}

} // namespace

ShaderFastPaths ResolveShaderFastPaths(const GraphicContext& graphics) {
	const bool      turnip = graphics.IsMesaTurnip();
	ShaderFastPaths paths {
	    .native_float_min_max = EnvToggle("KYTY_FAST_FMINMAX", turnip),
	    .robust_buffer_loads  = EnvToggle("KYTY_ROBUST_LOADS", turnip) &&
	                           graphics.optional_features.robust_buffer_access2_dword,
	    .lane_exec_branches   = EnvToggle("KYTY_LANE_BRANCHES", false),
	    .wqm_entry_lane_fold  = EnvToggle("KYTY_WQM_LANE_FOLD", false),
	    .wave_slices          = EnvToggle("KYTY_WAVE_SLICES", false),
	};
	Log::WriteToConsoleAndLog(fmt::format("Shader fast paths: native_float_min_max={} "
	                                      "robust_buffer_loads={} lane_exec_branches={} "
	                                      "wqm_entry_lane_fold={} wave_slices={}\n",
	                                      paths.native_float_min_max, paths.robust_buffer_loads,
	                                      paths.lane_exec_branches, paths.wqm_entry_lane_fold,
	                                      paths.wave_slices));
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
	const auto& filter = path == LaneFastPath::LaneBranches ? lane_branches : wqm_fold;
	if (!filter.only.empty() && !PipelineStats::Matches(filter.only, guest_hash)) return false;
	return filter.skip.empty() || !PipelineStats::Matches(filter.skip, guest_hash);
}

} // namespace Libs::Graphics
