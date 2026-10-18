#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDEBUGLABEL_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDEBUGLABEL_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics {

// Command-buffer debug regions named after the guest shader ("cs_48bc4ef0........"), so the
// validation layer's synchronization reports (debug_region / prior_debug_region) and GPU-AV
// errors say which dispatch or draw was involved. Enabled for KYTY_VK_VALIDATION runs only;
// otherwise a label costs one relaxed load.
void SetDebugLabelsEnabled(bool enabled);
[[nodiscard]] bool DebugLabelsEnabled();

class ScopedDebugLabel {
public:
	// "<kind>_<hash>", or "<kind>_<hash>_<second_hash>" (e.g. draw_<vs>_<ps>) when second_hash != 0.
	ScopedDebugLabel(vk::CommandBuffer buffer, const char* kind, uint64_t shader_hash,
	                 uint64_t second_hash = 0) {
		if (!DebugLabelsEnabled() || buffer == nullptr ||
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBeginDebugUtilsLabelEXT == nullptr) {
			return;
		}
		char name[80];
		if (second_hash != 0) {
			std::snprintf(name, sizeof(name), "%s_%016" PRIx64 "_%016" PRIx64, kind, shader_hash,
			              second_hash);
		} else {
			std::snprintf(name, sizeof(name), "%s_%016" PRIx64, kind, shader_hash);
		}
		vk::DebugUtilsLabelEXT label {};
		label.pLabelName = name;
		buffer.beginDebugUtilsLabelEXT(&label);
		m_buffer = buffer;
	}
	~ScopedDebugLabel() {
		if (m_buffer != nullptr) {
			m_buffer.endDebugUtilsLabelEXT();
		}
	}
	ScopedDebugLabel(const ScopedDebugLabel&)            = delete;
	ScopedDebugLabel& operator=(const ScopedDebugLabel&) = delete;

private:
	vk::CommandBuffer m_buffer = nullptr;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDEBUGLABEL_H_ */
