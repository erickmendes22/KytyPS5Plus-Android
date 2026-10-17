#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export

#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

struct VulkanImage;

inline constexpr uint32_t VULKAN_TARGET_API_VERSION = VK_API_VERSION_1_3;

// Optional device features, probed once when the logical device is created. A false flag never
// rejects a device; the renderer and shader backend read the flag and take a documented fallback.
struct OptionalDeviceFeatures {
	bool depth_clip_enable           = false; // VK_EXT_depth_clip_enable: depthClipEnable
	bool color_write_enable          = false; // VK_EXT_color_write_enable: colorWriteEnable
	bool fragment_shader_barycentric = false; // VK_KHR_fragment_shader_barycentric
	bool depth_bounds                = false; // core depthBounds
	bool image_view_min_lod          = false; // VK_EXT_image_view_min_lod: minLod
	bool shader_float64              = false; // core shaderFloat64 (enabled with FP64 signed zero)
	// Vulkan 1.2 shaderSharedInt64Atomics plus workgroupMemoryExplicitLayout.
	bool shader_shared_int64_atomics             = false;
	bool shader_signed_zero_inf_nan_preserve_f32 = false; // float controls property
	bool geometry_shader                         = false; // core geometryShader
	bool geometry_point_size = false; // core shaderTessellationAndGeometryPointSize
	// With geometry_shader: vec4 varying locations a geometry stage can pass from the last
	// pre-rasterization stage to the fragment stage (see ShaderDeviceFeatures).
	uint32_t geometry_varying_locations = 0;
	// VK_EXT_robustness2 nullDescriptor. Without it, unbound textures bind a 1x1 placeholder image.
	bool null_descriptor = false;
	// Vulkan 1.2 shaderStorageImageArrayNonUniformIndexing (see ShaderDeviceFeatures).
	bool storage_image_array_non_uniform_indexing = false;
	// VK_EXT_robustness2 robustBufferAccess2 with robustStorageBufferAccessSizeAlignment <= 4:
	// every out-of-range 32-bit storage buffer read returns zero.
	bool robust_buffer_access2_dword = false;
};

struct GraphicContext {
	vk::Instance                       instance                              = nullptr;
	vk::DebugUtilsMessengerEXT         debug_messenger                       = nullptr;
	vk::PhysicalDevice                 physical_device                       = nullptr;
	vk::PhysicalDeviceProperties       physical_device_properties            = {};
	vk::PhysicalDeviceMemoryProperties physical_device_memory_properties     = {};
	vk::Device                         device                                = nullptr;
	VmaAllocator                       allocator                             = nullptr;
	bool                               memory_budget_ext_enabled             = false;
	bool                               compute_subgroup_size_control_enabled = false;
	bool                               sample_rate_shading_enabled           = false;
	bool                               shader_image_int64_atomics_enabled    = false;
	// bool fp64_denorm_preserve = false; // Temporarily disabled.
	bool                               attachment_feedback_loop_enabled      = false;
	bool                               provoking_vertex_last_enabled         = false;
	bool                               maintenance8_enabled                  = false;
	// VkPhysicalDeviceDriverProperties::driverID of the selected device.
	vk::DriverId                       driver_id                             = {};
	// VK_KHR_pipeline_executable_properties, enabled only for KYTY_PIPELINE_STATS.
	bool                               pipeline_executable_info_enabled      = false;
	// VK_EXT_image_compression_control, enabled only for KYTY_UBWC_PROBE.
	bool                               image_compression_probe_enabled       = false;
	bool                               supports_block_texel_view              = false;
	bool                                      mesh_shader_enabled                   = false;
	vk::PhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties                = {};
	uint32_t                           subgroup_size                         = 0;
	uint32_t                           min_subgroup_size                     = 0;
	uint32_t                           max_subgroup_size                     = 0;
	uint32_t                           max_push_descriptors                  = 0;
	vk::ShaderStageFlags               required_subgroup_size_stages         = {};
	vk::ShaderStageFlags               subgroup_supported_stages             = {};
	vk::SubgroupFeatureFlags           subgroup_supported_operations         = {};
	OptionalDeviceFeatures             optional_features                     = {};
	Common::Mutex                      queue_mutex;
	uint32_t                           queue_family = static_cast<uint32_t>(-1);
	vk::Queue                          queue        = nullptr;

	[[nodiscard]] const vk::PhysicalDeviceProperties& GetPhysicalDeviceProperties() const {
		return physical_device_properties;
	}

	[[nodiscard]] const vk::PhysicalDeviceMemoryProperties&
	GetPhysicalDeviceMemoryProperties() const {
		return physical_device_memory_properties;
	}

	[[nodiscard]] vk::FormatProperties GetFormatProperties(vk::Format format) const {
		std::scoped_lock lock(m_format_properties_mutex);
		auto [it, inserted] = m_format_properties.try_emplace(format);
		if (inserted) {
			physical_device.getFormatProperties(format, &it->second);
		}
		return it->second;
	}

	[[nodiscard]] vk::Result GetImageFormatProperties(vk::Format format, vk::ImageType type,
	                                                  vk::ImageTiling            tiling,
	                                                  vk::ImageUsageFlags        usage,
	                                                  vk::ImageCreateFlags       flags,
	                                                  vk::ImageFormatProperties* properties) const {
		using Key = std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
		                       vk::ImageCreateFlags>;
		std::scoped_lock lock(m_image_format_properties_mutex);
		auto [it, inserted] =
		    m_image_format_properties.try_emplace(Key {format, type, tiling, usage, flags});
		if (inserted) {
			it->second.first = physical_device.getImageFormatProperties(format, type, tiling, usage,
			                                                            flags, &it->second.second);
		}
		if (properties != nullptr) {
			*properties = it->second.second;
		}
		return it->second.first;
	}

	[[nodiscard]] bool IsMesaTurnip() const noexcept {
		return driver_id == vk::DriverId::eMesaTurnip;
	}

	[[nodiscard]] bool SupportsComputeWave64() const noexcept {
		return subgroup_size == 64u || compute_subgroup_size_control_enabled;
	}

	// Whether invocations of the stage may use the subgroup ballot and shuffle operations the
	// shader recompiler emits for wave-level GCN semantics.
	[[nodiscard]] bool SubgroupOperationSupported(vk::SubgroupFeatureFlagBits operation) const noexcept {
		return static_cast<bool>(subgroup_supported_operations & operation);
	}

	[[nodiscard]] bool SubgroupOpsSupported(vk::ShaderStageFlagBits stage) const noexcept {
		constexpr auto required =
		    vk::SubgroupFeatureFlagBits::eBasic | vk::SubgroupFeatureFlagBits::eBallot |
		    vk::SubgroupFeatureFlagBits::eShuffle;
		return (subgroup_supported_stages & stage) == stage &&
		       (subgroup_supported_operations & required) == required;
	}

	[[nodiscard]] vk::DeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}

	[[nodiscard]] bool CreateAllocator();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	[[nodiscard]] bool     CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image);
	void                   DeleteImage(VulkanImage& image);

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;

private:
	mutable std::mutex                                 m_format_properties_mutex;
	mutable std::map<vk::Format, vk::FormatProperties> m_format_properties;
	mutable std::mutex                                 m_image_format_properties_mutex;
	mutable std::map<std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
	                            vk::ImageCreateFlags>,
	                 std::pair<vk::Result, vk::ImageFormatProperties>>
	    m_image_format_properties;
};

struct VulkanImageState {
	vk::PipelineStageFlags2 pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	vk::AccessFlags2        access_mask = vk::AccessFlagBits2::eNone;
	vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
};

struct VulkanImage {
	VulkanImage() = default;
	KYTY_CLASS_NO_COPY(VulkanImage);

	vk::Format                    format      = vk::Format::eUndefined;
	vk::ImageType                 image_type  = vk::ImageType::e2D;
	vk::Extent3D                  extent      = {1, 1, 1};
	uint32_t                      layers      = 1;
	uint32_t                      mip_levels  = 1;
	uint32_t                      samples     = 1;
	vk::ImageUsageFlags           usage       = {};
	vk::ImageCreateFlags          flags       = {};
	vk::Image                     image       = nullptr;
	VulkanImageState              state;
	std::vector<VulkanImageState> subresource_states;
	VmaAllocation                allocation = nullptr;
};



} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
