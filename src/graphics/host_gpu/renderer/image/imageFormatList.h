#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEFORMATLIST_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEFORMATLIST_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <functional>
#include <optional>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// Mutable color images get a VkImageFormatListCreateInfo naming every format their views may use
// (all supported formats of the same size class, ImageViewOps::FormatsCompatible). Without a
// list, Turnip must assume any format and turns UBWC compression off (tu_image.cc,
// format_list_ubwc_possible); with one whose formats all allow UBWC, Adreno 7xx/8xx keep it.
//
//   KYTY_IMAGE_FORMAT_LIST=1|0   force on / off; default: on for Mesa Turnip only.
namespace ImageFormatList {

[[nodiscard]] inline bool Resolve(std::optional<bool> configured, vk::DriverId driver) {
	return configured.value_or(driver == vk::DriverId::eMesaTurnip);
}
[[nodiscard]] bool Enabled(const GraphicContext& graphics);

// The view formats for a mutable image of `base`: `base` first, then every core color format of
// the same class that `supported` accepts. E5B9G9R9 (never UBWC on Turnip) is listed only as its
// own base. Empty for depth/stencil and block-compressed bases (left without a list).
[[nodiscard]] std::vector<vk::Format>
CompatibleViewFormats(vk::Format base, const std::function<bool(vk::Format)>& supported);

// Whether a view of `view` is covered by a list built for `base`.
[[nodiscard]] bool Covers(vk::Format base, vk::Format view);

} // namespace ImageFormatList
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEFORMATLIST_H_ */
