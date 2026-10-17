#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_UBWCPROBE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_UBWCPROBE_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct GraphicContext;

// KYTY_UBWC_PROBE=1: after each guest image is created, asks the driver whether mip 0 is
// compressed (VK_EXT_image_compression_control, VkImageCompressionPropertiesEXT; Turnip answers
// with its UBWC state) and appends one line per image to _perf_ubwc.txt, plus a running total.
namespace UbwcProbe {

[[nodiscard]] bool Requested();
void Record(const GraphicContext& graphics, vk::Image image, const vk::ImageCreateInfo& create,
            uint32_t view_format_count);

} // namespace UbwcProbe
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_UBWCPROBE_H_ */
