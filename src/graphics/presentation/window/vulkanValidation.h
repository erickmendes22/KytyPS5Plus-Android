#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANVALIDATION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANVALIDATION_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <string>
#include <vector>

// KYTY_VK_VALIDATION: diagnostic runs with the Khronos validation layer (packaged into the APK's
// native libs with -PkytyValidationLayer=<.so>, or installed on desktop). Unlike
// --vulkan-validation, messages are never fatal: they are deduplicated, counted and logged
// ("[vvl]" lines; logcat tag KytyVVL), with a summary every 30 s and at a GPU fatal error.
//
//   KYTY_VK_VALIDATION=1             core + synchronization validation (GPU-AV is opt-in: it
//                                    hangs Astro Bot's boot on Turnip v32)
//   KYTY_VK_VALIDATION=core+sync+... any of (',' or '+' separated): core, sync, gpuav (GPU-AV
//                                    incl. buffer device address OOB checks), limit (keep the
//                                    layer's 10-per-VUID cap; by default every message is counted)
namespace Libs::Graphics::VulkanValidation {

struct Options {
	bool requested = false;
	bool core      = false;
	bool sync      = false;
	bool gpuav     = false;
	bool limit     = false;
};

// Parsed once from the environment.
[[nodiscard]] const Options& Requested();

// VkLayerSettingsCreateInfoEXT for VK_LAYER_KHRONOS_validation built from Requested(); the
// object owns the setting values, so it must outlive vkCreateInstance.
class LayerSettings {
public:
	LayerSettings();
	[[nodiscard]] const vk::LayerSettingsCreateInfoEXT* Info() const { return &m_info; }

private:
	void AddBool(const char* name, bool value);

	std::vector<vk::Bool32>           m_bools;
	std::vector<vk::LayerSettingEXT>  m_settings;
	vk::LayerSettingsCreateInfoEXT    m_info {};
};

// Debug messenger callback for KYTY_VK_VALIDATION runs (dedup + counts, never fatal).
VKAPI_ATTR vk::Bool32 VKAPI_CALL MessengerCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT      severity,
    vk::DebugUtilsMessageTypeFlagsEXT             types,
    const vk::DebugUtilsMessengerCallbackDataEXT* data, void* user_data);

// Logs the deduplicated message table (most frequent first). Also registered as a GPU fatal hook.
void LogSummary(const char* why);

// The dedup key: message id name plus the text with handles, addresses and counters masked, so
// one hazard on one resource pair is one entry however often it repeats. Exposed for tests.
[[nodiscard]] std::string DedupKey(const char* id_name, const char* message);

} // namespace Libs::Graphics::VulkanValidation

#endif /* EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANVALIDATION_H_ */
