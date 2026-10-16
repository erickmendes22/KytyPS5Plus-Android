// LogDeviceCaps: names the Vulkan driver in use and, with --print-vulkan-caps, dumps what the
// selected device offers. Useful to compare the stock vendor driver with a custom one (Turnip).

#include "common/logging/log.h"
#include "graphics/presentation/window/vulkanDriverLoader.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace Libs::Graphics::VulkanDriver {

namespace {

void Report(const std::string& line) {
	Log::WriteToConsoleAndLog(line + "\n");
}

bool Has(const std::vector<vk::ExtensionProperties>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(), [name](const auto& ext) {
		return std::strcmp(ext.extensionName.data(), name) == 0;
	});
}

// Links |next| at the head of |features|' pNext chain when the device has |extension|.
template <typename T>
void ChainIf(bool present, vk::PhysicalDeviceFeatures2& features, T& next) {
	if (present) {
		next.pNext     = features.pNext;
		features.pNext = &next;
	}
}

const char* YesNo(vk::Bool32 value) {
	return value == VK_TRUE ? "yes" : "no";
}

void ReportFeatures(vk::PhysicalDevice                          device,
                    const std::vector<vk::ExtensionProperties>& extensions) {
	vk::PhysicalDeviceVulkan13Features features13 {};
	vk::PhysicalDeviceVulkan12Features features12 {};
	features13.pNext = &features12;
	vk::PhysicalDeviceFeatures2 features2 {};
	features2.pNext = &features13;

	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric {};
	vk::PhysicalDeviceMeshShaderFeaturesEXT                mesh {};
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup_layout {};
	vk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT    image_int64 {};
	vk::PhysicalDeviceRobustness2FeaturesEXT               robustness2 {};
	const bool has_barycentric = Has(extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
	const bool has_mesh        = Has(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
	const bool has_workgroup =
	    Has(extensions, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
	const bool has_image_int64 = Has(extensions, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
	const bool has_robustness2 = Has(extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
	ChainIf(has_barycentric, features2, barycentric);
	ChainIf(has_mesh, features2, mesh);
	ChainIf(has_workgroup, features2, workgroup_layout);
	ChainIf(has_image_int64, features2, image_int64);
	ChainIf(has_robustness2, features2, robustness2);
	device.getFeatures2(&features2);

	const auto& core = features2.features;
	Report(fmt::format("Vulkan caps: fragmentShaderBarycentric={} meshShader={} taskShader={} "
	                   "geometryShader={} tessellationShader={}",
	                   YesNo(barycentric.fragmentShaderBarycentric), YesNo(mesh.meshShader),
	                   YesNo(mesh.taskShader), YesNo(core.geometryShader),
	                   YesNo(core.tessellationShader)));
	Report(fmt::format("Vulkan caps: shaderFloat64={} shaderInt64={} shaderInt16={} "
	                   "shaderFloat16={} shaderInt8={}",
	                   YesNo(core.shaderFloat64), YesNo(core.shaderInt64), YesNo(core.shaderInt16),
	                   YesNo(features12.shaderFloat16), YesNo(features12.shaderInt8)));
	Report(fmt::format("Vulkan caps: shaderSharedInt64Atomics={} shaderBufferInt64Atomics={} "
	                   "workgroupMemoryExplicitLayout={} shaderImageInt64Atomics={}",
	                   YesNo(features12.shaderSharedInt64Atomics),
	                   YesNo(features12.shaderBufferInt64Atomics),
	                   YesNo(workgroup_layout.workgroupMemoryExplicitLayout),
	                   YesNo(image_int64.shaderImageInt64Atomics)));
	Report(fmt::format("Vulkan caps: robustImageAccess={} robustBufferAccess2={} "
	                   "robustImageAccess2={} nullDescriptor={} subgroupSizeControl={} "
	                   "depthBounds={} dualSrcBlend={}",
	                   YesNo(features13.robustImageAccess), YesNo(robustness2.robustBufferAccess2),
	                   YesNo(robustness2.robustImageAccess2), YesNo(robustness2.nullDescriptor),
	                   YesNo(features13.subgroupSizeControl), YesNo(core.depthBounds),
	                   YesNo(core.dualSrcBlend)));
}

void ReportProperties(vk::PhysicalDevice device) {
	vk::PhysicalDeviceSubgroupSizeControlProperties subgroup_control {};
	vk::PhysicalDeviceVulkan11Properties            properties11 {};
	properties11.pNext = &subgroup_control;
	vk::PhysicalDeviceFloatControlsProperties float_controls {};
	float_controls.pNext = &properties11;
	vk::PhysicalDeviceProperties2 properties2 {};
	properties2.pNext = &float_controls;
	device.getProperties2(&properties2);

	const auto& limits = properties2.properties.limits;
	Report(fmt::format("Vulkan caps: subgroupSize={} min={} max={} maxComputeSharedMemory={} "
	                   "maxComputeWorkGroupInvocations={} maxPushConstants={}",
	                   properties11.subgroupSize, subgroup_control.minSubgroupSize,
	                   subgroup_control.maxSubgroupSize, limits.maxComputeSharedMemorySize,
	                   limits.maxComputeWorkGroupInvocations, limits.maxPushConstantsSize));
	Report(fmt::format("Vulkan caps: float64 signedZeroInfNanPreserve={} denormPreserve={} "
	                   "roundingModeRTE={}; float32 signedZeroInfNanPreserve={}",
	                   YesNo(float_controls.shaderSignedZeroInfNanPreserveFloat64),
	                   YesNo(float_controls.shaderDenormPreserveFloat64),
	                   YesNo(float_controls.shaderRoundingModeRTEFloat64),
	                   YesNo(float_controls.shaderSignedZeroInfNanPreserveFloat32)));
}

} // namespace

void LogDeviceCaps(vk::PhysicalDevice device) {
	vk::PhysicalDeviceDriverProperties driver {};
	vk::PhysicalDeviceProperties2      properties2 {};
	properties2.pNext = &driver;
	device.getProperties2(&properties2);
	const auto& properties = properties2.properties;

	Report(fmt::format("GPU driver in use: {} {} ({}; {}), device {} [{:04x}:{:08x}], Vulkan {}.{}.{}",
	                   driver.driverName.data(), driver.driverInfo.data(),
	                   vk::to_string(driver.driverID),
	                   CustomDriverActive() ? "custom driver" : "system driver",
	                   properties.deviceName.data(), properties.vendorID, properties.deviceID,
	                   VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
	                   VK_VERSION_PATCH(properties.apiVersion)));

	if (!PrintCapsEnabled()) {
		return;
	}

	auto extensions = EnumerateVulkan<vk::ExtensionProperties>(
	    "vkEnumerateDeviceExtensionProperties",
	    [&](uint32_t* count, vk::ExtensionProperties* values) {
		    return device.enumerateDeviceExtensionProperties(nullptr, count, values);
	    });
	std::sort(extensions.begin(), extensions.end(), [](const auto& a, const auto& b) {
		return std::strcmp(a.extensionName.data(), b.extensionName.data()) < 0;
	});

	Report(fmt::format("Vulkan caps: driverVersion=0x{:08x} conformance={}.{}.{}.{} extensions={}",
	                   properties.driverVersion, driver.conformanceVersion.major,
	                   driver.conformanceVersion.minor, driver.conformanceVersion.subminor,
	                   driver.conformanceVersion.patch, extensions.size()));
	ReportFeatures(device, extensions);
	ReportProperties(device);
	for (const auto& ext: extensions) {
		Report(fmt::format("Vulkan caps ext: {} v{}", ext.extensionName.data(), ext.specVersion));
	}
}

} // namespace Libs::Graphics::VulkanDriver
