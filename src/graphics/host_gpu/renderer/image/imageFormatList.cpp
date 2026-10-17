#include "graphics/host_gpu/renderer/image/imageFormatList.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageView.h"

#include <cstdlib>
#include <cstring>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics::ImageFormatList {

namespace {

// Core formats run from VK_FORMAT_R4G4_UNORM_PACK8 (1) to VK_FORMAT_ASTC_12x12_SRGB_BLOCK (184).
constexpr int kFirstCoreFormat = 1;
constexpr int kLastCoreFormat  = 184;

std::optional<bool> Configured() {
	const char* value = std::getenv("KYTY_IMAGE_FORMAT_LIST");
	if (value == nullptr || *value == '\0') {
		return std::nullopt;
	}
	return std::strcmp(value, "0") != 0;
}

bool Listable(vk::Format format) {
	return format != vk::Format::eUndefined && vk::isColor(format) && !vk::isCompressed(format);
}

} // namespace

bool Enabled(const GraphicContext& graphics) {
	static const bool enabled = [&graphics] {
		const bool value = Resolve(Configured(), graphics.driver_id);
		LOGF("Image format lists for mutable images: %s\n", value ? "on" : "off");
		return value;
	}();
	return enabled;
}

std::vector<vk::Format> CompatibleViewFormats(vk::Format base,
                                              const std::function<bool(vk::Format)>& supported) {
	if (!Listable(base)) {
		return {};
	}
	std::vector<vk::Format> formats {base};
	for (int value = kFirstCoreFormat; value <= kLastCoreFormat; value++) {
		const auto format = static_cast<vk::Format>(value);
		if (format == base || !Listable(format) || !Covers(base, format) || !supported(format)) {
			continue;
		}
		formats.push_back(format);
	}
	return formats;
}

bool Covers(vk::Format base, vk::Format view) {
	if (view == base) {
		return true;
	}
	if (view == vk::Format::eE5B9G9R9UfloatPack32) {
		return false;
	}
	return ImageViewOps::FormatsCompatible(base, view);
}

} // namespace Libs::Graphics::ImageFormatList
