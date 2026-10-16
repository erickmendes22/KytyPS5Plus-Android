#include "graphics/host_gpu/renderer/pipeline/hostShaderQuirks.h"

#include "common/logging/log.h"

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

} // namespace Libs::Graphics
