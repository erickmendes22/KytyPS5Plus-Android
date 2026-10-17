#include "graphics/host_gpu/renderer/pipeline/hostShaderQuirks.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdlib>
#include <fmt/format.h>

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
	};
	Log::WriteToConsoleAndLog(fmt::format("Shader fast paths: native_float_min_max={} "
	                                      "robust_buffer_loads={}\n",
	                                      paths.native_float_min_max, paths.robust_buffer_loads));
	return paths;
}

} // namespace Libs::Graphics
