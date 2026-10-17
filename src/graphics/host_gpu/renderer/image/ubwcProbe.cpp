#include "graphics/host_gpu/renderer/image/ubwcProbe.h"

#include "graphics/host_gpu/graphicContext.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Libs::Graphics::UbwcProbe {

bool Requested() {
	static const bool requested = [] {
		const char* value = std::getenv("KYTY_UBWC_PROBE");
		return value != nullptr && std::strcmp(value, "0") != 0;
	}();
	return requested;
}

void Record(const GraphicContext& graphics, vk::Image image, const vk::ImageCreateInfo& create,
            uint32_t view_format_count) {
	if (!Requested() || !graphics.image_compression_probe_enabled || image == nullptr) {
		return;
	}
	const auto query = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageSubresourceLayout2EXT;
	if (query == nullptr) {
		return;
	}
	const bool depth = create.format == vk::Format::eD16Unorm ||
	                   create.format == vk::Format::eD32Sfloat ||
	                   create.format == vk::Format::eD16UnormS8Uint ||
	                   create.format == vk::Format::eD24UnormS8Uint ||
	                   create.format == vk::Format::eD32SfloatS8Uint ||
	                   create.format == vk::Format::eX8D24UnormPack32;
	VkImageSubresource2 subresource {};
	subresource.sType            = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2;
	subresource.imageSubresource = {depth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
	                                      : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
	                                0, 0};
	VkImageCompressionPropertiesEXT compression {};
	compression.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT;
	VkSubresourceLayout2 layout {};
	layout.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2;
	layout.pNext = &compression;
	query(graphics.device, image, &subresource, &layout);
	const bool compressed =
	    (compression.imageCompressionFlags & VK_IMAGE_COMPRESSION_DISABLED_EXT) == 0;

	static std::mutex mutex;
	static uint64_t   total      = 0;
	static uint64_t   ubwc       = 0;
	static uint64_t   big_total  = 0;
	static uint64_t   big_ubwc   = 0;
	std::scoped_lock  lock(mutex);
	static std::FILE* out = std::fopen("_perf_ubwc.txt", "w");
	if (out == nullptr) {
		return;
	}
	const bool big = create.extent.width >= 512 && create.extent.height >= 256;
	total++;
	ubwc += compressed ? 1u : 0u;
	big_total += big ? 1u : 0u;
	big_ubwc += big && compressed ? 1u : 0u;
	std::fprintf(out,
	             "%s fmt=%d %ux%ux%u mips=%u layers=%u samples=%u flags=0x%x usage=0x%x "
	             "view_formats=%u | total %llu/%llu, >=512x256 %llu/%llu\n",
	             compressed ? "UBWC" : "none", static_cast<int>(create.format), create.extent.width,
	             create.extent.height, create.extent.depth, create.mipLevels, create.arrayLayers,
	             static_cast<uint32_t>(create.samples),
	             static_cast<vk::ImageCreateFlags::MaskType>(create.flags),
	             static_cast<vk::ImageUsageFlags::MaskType>(create.usage), view_format_count,
	             static_cast<unsigned long long>(ubwc), static_cast<unsigned long long>(total),
	             static_cast<unsigned long long>(big_ubwc),
	             static_cast<unsigned long long>(big_total));
	std::fflush(out);
}

} // namespace Libs::Graphics::UbwcProbe
