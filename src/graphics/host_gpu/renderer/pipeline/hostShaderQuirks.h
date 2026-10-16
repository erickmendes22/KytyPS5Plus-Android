#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/deviceQuirks.h"

namespace Libs::Graphics {

[[nodiscard]] ShaderDriverInfo QueryShaderDriverInfo(vk::PhysicalDevice physical_device);

// Quirks detected for the device, then the KYTY_SHADER_QUIRKS overrides ("name,-name,none").
// Logs the result.
[[nodiscard]] ShaderDeviceQuirks ResolveShaderDeviceQuirks(vk::PhysicalDevice physical_device);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HOSTSHADERQUIRKS_H_ */
