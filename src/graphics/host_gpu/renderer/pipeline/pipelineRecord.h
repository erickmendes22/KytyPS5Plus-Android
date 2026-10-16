#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINERECORD_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINERECORD_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

// JSON form of a Vulkan pipeline create call, shared by the emulator (--dump-shaders writes it)
// and tools/shader_checker (rebuilds the same call on another process). Only the state Kyty puts
// into its create infos is encoded; Encode rejects pNext structures it does not know, so a new
// renderer feature cannot silently produce records that rebuild a different pipeline.
namespace Libs::Graphics::PipelineRecord {

class Reader;

inline constexpr int Version = 1;

struct LayoutState {
	std::span<const vk::DescriptorSetLayoutBinding> bindings;
	vk::DescriptorSetLayoutCreateFlags              flags;
	vk::PushConstantRange                           push_constants;
};

struct ModuleName {
	std::string file; // dumped SPIR-V, relative to the dump root; empty when unknown
	uint64_t    guest_hash = 0;
};
using ModuleNamer = std::function<ModuleName(vk::ShaderModule)>;

// Both return std::nullopt (and set *error) when the create info holds state the record format
// does not cover.
std::optional<nlohmann::json> EncodeCompute(const vk::ComputePipelineCreateInfo& info,
                                            const LayoutState& layout, const ModuleNamer& namer,
                                            std::string* error);
std::optional<nlohmann::json> EncodeGraphics(const vk::GraphicsPipelineCreateInfo& info,
                                             const LayoutState& layout, const ModuleNamer& namer,
                                             std::string* error);

struct Stage {
	vk::ShaderStageFlagBits stage                  = vk::ShaderStageFlagBits::eCompute;
	std::string             spv;
	std::string             entry                  = "main";
	uint32_t                required_subgroup_size = 0;
	uint64_t                guest_hash             = 0;
};

// Decoded record. Build* return create infos that point into this object and the caller's
// modules, so the Pipeline must outlive the Vulkan call.
class Pipeline {
public:
	static std::optional<Pipeline> Decode(const nlohmann::json& record, std::string* error);

	[[nodiscard]] bool                      IsCompute() const { return m_compute; }
	[[nodiscard]] const std::vector<Stage>& Stages() const { return m_stages; }
	[[nodiscard]] std::vector<Stage>&       Stages() { return m_stages; }
	[[nodiscard]] vk::DescriptorSetLayoutCreateInfo LayoutInfo() const;
	[[nodiscard]] const vk::PushConstantRange&      PushConstants() const { return m_push; }

	vk::ComputePipelineCreateInfo  BuildCompute(vk::ShaderModule module, vk::PipelineLayout layout);
	vk::GraphicsPipelineCreateInfo BuildGraphics(std::span<const vk::ShaderModule> modules,
	                                             vk::PipelineLayout                layout);

	Pipeline(const Pipeline&)            = delete;
	Pipeline& operator=(const Pipeline&) = delete;
	Pipeline(Pipeline&&)                 = default;
	Pipeline& operator=(Pipeline&&)      = default;

private:
	Pipeline() = default;
	void BuildStages(std::span<const vk::ShaderModule> modules);
	void DecodeGraphics(const nlohmann::json& record, Reader& reader);

	bool                                        m_compute = true;
	std::vector<Stage>                          m_stages;
	std::vector<vk::DescriptorSetLayoutBinding> m_bindings;
	vk::DescriptorSetLayoutCreateFlags          m_layout_flags;
	vk::PushConstantRange                       m_push;
	vk::PipelineCreateFlags                     m_flags;

	// Graphics state; std::optional marks structures that were absent (nullptr) in the record.
	std::vector<vk::VertexInputBindingDescription>   m_vertex_bindings;
	std::vector<vk::VertexInputAttributeDescription> m_vertex_attributes;
	std::optional<vk::PipelineVertexInputStateCreateInfo>    m_vertex_input;
	std::optional<vk::PipelineInputAssemblyStateCreateInfo>  m_input_assembly;
	std::optional<vk::PipelineTessellationStateCreateInfo>   m_tessellation;
	std::optional<vk::PipelineViewportStateCreateInfo>       m_viewport;
	std::optional<vk::PipelineViewportDepthClipControlCreateInfoEXT> m_depth_clip_control;
	std::optional<vk::PipelineRasterizationStateCreateInfo>  m_rasterization;
	std::optional<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>       m_depth_clip;
	std::optional<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT> m_provoking;
	std::optional<vk::PipelineMultisampleStateCreateInfo>    m_multisample;
	std::optional<vk::PipelineDepthStencilStateCreateInfo>   m_depth_stencil;
	std::optional<vk::PipelineColorBlendStateCreateInfo>     m_color_blend;
	std::vector<vk::PipelineColorBlendAttachmentState>       m_blend_attachments;
	std::optional<std::vector<vk::Bool32>>                   m_color_write_enables;
	vk::PipelineColorWriteCreateInfoEXT                      m_color_write;
	std::vector<vk::DynamicState>                            m_dynamic_states;
	vk::PipelineDynamicStateCreateInfo                       m_dynamic;
	std::optional<vk::PipelineRenderingCreateInfo>           m_rendering;
	std::vector<vk::Format>                                  m_color_formats;

	std::vector<vk::PipelineShaderStageCreateInfo>                     m_stage_infos;
	std::vector<vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo> m_subgroup_infos;
};

} // namespace Libs::Graphics::PipelineRecord

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINERECORD_H_ */
