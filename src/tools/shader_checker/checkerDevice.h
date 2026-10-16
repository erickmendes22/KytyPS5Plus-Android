#ifndef KYTY_TOOLS_SHADER_CHECKER_CHECKERDEVICE_H_
#define KYTY_TOOLS_SHADER_CHECKER_CHECKERDEVICE_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>
#include <string>
#include <vector>

namespace ShaderChecker {

// Headless Vulkan device configured like the emulator's: the same device extensions (when the
// driver has them) and every feature the driver reports for the enabled feature structures. A
// superset of the emulator's features never removes code paths the driver compiles, so a module
// that fails here also fails in the emulator; extra features only matter for shaders that
// already use them.
class Device {
public:
	static std::unique_ptr<Device> Create(int gpu_index, std::string* error);
	~Device();

	Device(const Device&)            = delete;
	Device& operator=(const Device&) = delete;

	[[nodiscard]] vk::Device                          Handle() const { return m_device; }
	[[nodiscard]] const vk::PhysicalDeviceProperties& Properties() const { return m_properties; }
	[[nodiscard]] const vk::PhysicalDeviceDriverProperties& Driver() const { return m_driver; }
	[[nodiscard]] const std::vector<std::string>& Extensions() const { return m_extensions; }

private:
	Device() = default;

	vk::detail::DynamicLoader                m_loader;
	vk::Instance                       m_instance = nullptr;
	vk::PhysicalDevice                 m_physical = nullptr;
	vk::Device                         m_device   = nullptr;
	vk::PhysicalDeviceProperties       m_properties {};
	vk::PhysicalDeviceDriverProperties m_driver {};
	std::vector<std::string>           m_extensions;
};

} // namespace ShaderChecker

#endif /* KYTY_TOOLS_SHADER_CHECKER_CHECKERDEVICE_H_ */
