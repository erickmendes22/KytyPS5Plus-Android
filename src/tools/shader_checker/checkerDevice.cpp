#include "tools/shader_checker/checkerDevice.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace ShaderChecker {

namespace {

// Device extensions the emulator enables (graphics/presentation/window/vulkanWindow.cpp), minus
// the swapchain. Each one is enabled here when the driver has it.
constexpr std::array kEmulatorExtensions = {
    VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_8_EXTENSION_NAME,
    VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME,
    VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
    VK_EXT_MESH_SHADER_EXTENSION_NAME,
    VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
    VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
    VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
    VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME,
    VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
    VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
    VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
    VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME,
    VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
};

// Feature structures queried and then enabled as reported. Kept alive while the device is created.
struct FeatureChain {
	vk::PhysicalDeviceFeatures2                                     features2 {};
	vk::PhysicalDeviceVulkan11Features                              v11 {};
	vk::PhysicalDeviceVulkan12Features                              v12 {};
	vk::PhysicalDeviceVulkan13Features                              v13 {};
	vk::PhysicalDeviceDepthClipControlFeaturesEXT                   depth_clip_control {};
	vk::PhysicalDeviceRobustness2FeaturesEXT                        robustness2 {};
	vk::PhysicalDeviceMaintenance8FeaturesKHR                       maintenance8 {};
	vk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT             image_atomic_int64 {};
	vk::PhysicalDeviceProvokingVertexFeaturesEXT                    provoking_vertex {};
	vk::PhysicalDeviceMeshShaderFeaturesEXT                         mesh {};
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR      workgroup_layout {};
	vk::PhysicalDeviceDepthClipEnableFeaturesEXT                    depth_clip_enable {};
	vk::PhysicalDeviceColorWriteEnableFeaturesEXT                   color_write {};
	vk::PhysicalDeviceImageViewMinLodFeaturesEXT                    min_lod {};
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR          barycentric {};
	vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT       feedback_layout {};
	vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedback_dynamic {};

	void Link(const std::vector<std::string>& extensions) {
		const auto has = [&](const char* name) {
			return std::ranges::find(extensions, name) != extensions.end();
		};
		void* chain = nullptr;
		const auto push = [&](auto& feature, bool enabled) {
			if (enabled) {
				feature.pNext = chain;
				chain         = &feature;
			}
		};
		push(v11, true);
		push(v12, true);
		push(v13, true);
		push(depth_clip_control, has(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME));
		push(robustness2, has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME));
		push(maintenance8, has(VK_KHR_MAINTENANCE_8_EXTENSION_NAME));
		push(image_atomic_int64, has(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME));
		push(provoking_vertex, has(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME));
		push(mesh, has(VK_EXT_MESH_SHADER_EXTENSION_NAME));
		push(workgroup_layout, has(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME));
		push(depth_clip_enable, has(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));
		push(color_write, has(VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME));
		push(min_lod, has(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME));
		push(barycentric, has(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME));
		push(feedback_layout, has(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME));
		push(feedback_dynamic, has(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME));
		features2.pNext = chain;
	}

	// Features the emulator never enables and that change how the driver treats the device.
	void DropUnwanted() {
		features2.features.robustBufferAccess = VK_TRUE; // emulator requires it
		v11.protectedMemory                   = VK_FALSE;
		mesh.multiviewMeshShader              = VK_FALSE;
		mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
		image_atomic_int64.sparseImageInt64Atomics  = VK_FALSE;
		provoking_vertex.transformFeedbackPreservesProvokingVertex = VK_FALSE;
	}
};

uint32_t FindQueueFamily(vk::PhysicalDevice device) {
	const auto families = device.getQueueFamilyProperties();
	for (uint32_t i = 0; i < families.size(); i++) {
		if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
			return i;
		}
	}
	return UINT32_MAX;
}

} // namespace

std::unique_ptr<Device> Device::Create(int gpu_index, std::string* error) {
	std::unique_ptr<Device> device(new Device());
	if (!device->m_loader.success()) {
		*error = "cannot load the Vulkan loader";
		return nullptr;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(
	    device->m_loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

	vk::ApplicationInfo app {};
	app.pApplicationName = "kyty-shader-checker";
	app.apiVersion       = VK_API_VERSION_1_3;
	vk::InstanceCreateInfo instance_info {};
	instance_info.pApplicationInfo = &app;
	if (const auto result = vk::createInstance(&instance_info, nullptr, &device->m_instance);
	    result != vk::Result::eSuccess) {
		*error = "vkCreateInstance: " + vk::to_string(result);
		return nullptr;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(device->m_instance);

	const auto [enum_result, physicals] = device->m_instance.enumeratePhysicalDevices();
	if (enum_result != vk::Result::eSuccess || physicals.empty()) {
		*error = "no Vulkan physical device";
		return nullptr;
	}
	const auto index =
	    gpu_index >= 0 && static_cast<size_t>(gpu_index) < physicals.size() ? gpu_index : 0;
	device->m_physical = physicals[index];

	vk::PhysicalDeviceProperties2 properties {};
	properties.pNext = &device->m_driver;
	device->m_physical.getProperties2(&properties);
	device->m_properties = properties.properties;

	const auto [ext_result, available] = device->m_physical.enumerateDeviceExtensionProperties();
	for (const char* name: kEmulatorExtensions) {
		if (std::ranges::any_of(available, [&](const vk::ExtensionProperties& e) {
			    return std::string_view(e.extensionName.data()) == name;
		    })) {
			device->m_extensions.emplace_back(name);
		}
	}

	auto chain = std::make_unique<FeatureChain>();
	chain->Link(device->m_extensions);
	device->m_physical.getFeatures2(&chain->features2);
	chain->DropUnwanted();

	const auto queue_family = FindQueueFamily(device->m_physical);
	if (queue_family == UINT32_MAX) {
		*error = "no graphics queue";
		return nullptr;
	}
	const float               priority = 1.0f;
	vk::DeviceQueueCreateInfo queue {};
	queue.queueFamilyIndex = queue_family;
	queue.queueCount       = 1;
	queue.pQueuePriorities = &priority;

	std::vector<const char*> names;
	for (const auto& e: device->m_extensions) {
		names.push_back(e.c_str());
	}
	vk::DeviceCreateInfo create {};
	create.pNext                   = &chain->features2;
	create.queueCreateInfoCount    = 1;
	create.pQueueCreateInfos       = &queue;
	create.enabledExtensionCount   = static_cast<uint32_t>(names.size());
	create.ppEnabledExtensionNames = names.data();
	if (const auto result = device->m_physical.createDevice(&create, nullptr, &device->m_device);
	    result != vk::Result::eSuccess) {
		*error = "vkCreateDevice: " + vk::to_string(result);
		return nullptr;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(device->m_device);
	return device;
}

Device::~Device() {
	if (m_device != nullptr) {
		m_device.destroy();
	}
	if (m_instance != nullptr) {
		m_instance.destroy();
	}
}

} // namespace ShaderChecker
