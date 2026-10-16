// Shader device quirk detection and the pipeline record format shared by --dump-shaders and
// shader_checker.

#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"
#include "graphics/shader/deviceQuirks.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ShaderCheckTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint32_t kQualcommProprietary = 8; // VK_DRIVER_ID_QUALCOMM_PROPRIETARY
constexpr uint32_t kMesaTurnip          = 18;
constexpr uint32_t kAmdProprietary      = 1;

void TestQuirkDetection() {
	Check(DetectShaderDeviceQuirks({.vendor_id = 0x5143, .driver_id = kQualcommProprietary})
	          .branchless_bounds_checks,
	      "Qualcomm proprietary driver must get branchless bounds checks");
	Check(DetectShaderDeviceQuirks({.vendor_id = 0x5143, .driver_id = kMesaTurnip}) ==
	          ShaderDeviceQuirks {},
	      "Turnip must keep the portable output");
	Check(DetectShaderDeviceQuirks({.vendor_id = 0x1002, .driver_id = kAmdProprietary}) ==
	          ShaderDeviceQuirks {},
	      "desktop drivers must keep the portable output");
	Check(DescribeShaderQuirks({}) == "none", "empty quirk set is described as none");
}

void TestQuirkOverrides() {
	ShaderDeviceQuirks quirks;
	std::string        error;
	Check(ApplyShaderQuirkOverrides("branchless_bounds_checks", quirks, &error) &&
	          quirks.branchless_bounds_checks,
	      "override enables a quirk");
	Check(DescribeShaderQuirks(quirks) == "branchless_bounds_checks", "enabled quirk is described");
	Check(ApplyShaderQuirkOverrides("-branchless_bounds_checks", quirks, &error) &&
	          !quirks.branchless_bounds_checks,
	      "override disables a quirk");
	quirks.branchless_bounds_checks = true;
	Check(ApplyShaderQuirkOverrides("none", quirks, &error) && quirks == ShaderDeviceQuirks {},
	      "none clears every quirk");
	Check(!ApplyShaderQuirkOverrides("no_such_quirk", quirks, &error) && !error.empty(),
	      "unknown quirk is rejected");
}

PipelineRecord::ModuleNamer Namer() {
	return [](vk::ShaderModule module) -> PipelineRecord::ModuleName {
		const auto raw = reinterpret_cast<uintptr_t>(static_cast<VkShaderModule>(module));
		return {.file = "spv/m" + std::to_string(raw) + ".spv", .guest_hash = raw * 0x1111};
	};
}

vk::ShaderModule FakeModule(uintptr_t value) {
	return vk::ShaderModule(reinterpret_cast<VkShaderModule>(value));
}

void TestComputeRecord() {
	const std::array<vk::DescriptorSetLayoutBinding, 2> bindings {{
	    {0, vk::DescriptorType::eStorageBuffer, 7, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {49, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	}};
	const PipelineRecord::LayoutState layout {
	    bindings, vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
	    {vk::ShaderStageFlagBits::eCompute, 0, 128}};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup {};
	subgroup.requiredSubgroupSize = 64;
	vk::ComputePipelineCreateInfo info {};
	info.stage.pNext  = &subgroup;
	info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	info.stage.module = FakeModule(5);
	info.stage.pName  = "main";
	std::string error;
	const auto  json = PipelineRecord::EncodeCompute(info, layout, Namer(), &error);
	Check(json.has_value(), "compute pipeline encodes");
	auto decoded = PipelineRecord::Pipeline::Decode(*json, &error);
	Check(decoded.has_value() && decoded->IsCompute(), "compute record decodes");
	Check(decoded->Stages().size() == 1 && decoded->Stages()[0].spv == "spv/m5.spv" &&
	          decoded->Stages()[0].guest_hash == 5 * 0x1111 &&
	          decoded->Stages()[0].required_subgroup_size == 64,
	      "compute stage round-trips");
	const auto layout_info = decoded->LayoutInfo();
	Check(layout_info.bindingCount == 2 && layout_info.pBindings[1].binding == 49 &&
	          layout_info.pBindings[0].descriptorCount == 7 &&
	          layout_info.flags == vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
	      "layout round-trips");
	Check(decoded->PushConstants().size == 128, "push constants round-trip");
	const auto rebuilt = decoded->BuildCompute(FakeModule(9), nullptr);
	Check(rebuilt.stage.module == FakeModule(9) && rebuilt.stage.pNext != nullptr,
	      "compute create info is rebuilt with the subgroup size");
}

void TestGraphicsRecord() {
	vk::PipelineShaderStageCreateInfo stages[2] {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = FakeModule(1);
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = FakeModule(2);
	stages[1].pName  = "main";
	const vk::VertexInputBindingDescription   binding {0, 16, vk::VertexInputRate::eVertex};
	const vk::VertexInputAttributeDescription attribute {0, 0, vk::Format::eR32G32Sfloat, 4};
	vk::PipelineVertexInputStateCreateInfo    vertex {};
	vertex.vertexBindingDescriptionCount   = 1;
	vertex.pVertexBindingDescriptions      = &binding;
	vertex.vertexAttributeDescriptionCount = 1;
	vertex.pVertexAttributeDescriptions    = &attribute;
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleStrip;
	vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control {};
	clip_control.negativeOneToOne = VK_TRUE;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.pNext = &clip_control;
	vk::PipelineRasterizationDepthClipStateCreateInfoEXT depth_clip {};
	depth_clip.depthClipEnable = VK_FALSE;
	vk::PipelineRasterizationStateCreateInfo raster {};
	raster.pNext     = &depth_clip;
	raster.cullMode  = vk::CullModeFlagBits::eBack;
	raster.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e4;
	vk::PipelineColorBlendAttachmentState blend_attachment {};
	blend_attachment.blendEnable         = VK_TRUE;
	blend_attachment.srcColorBlendFactor = vk::BlendFactor::eSrc1Alpha;
	blend_attachment.colorWriteMask      = vk::ColorComponentFlagBits::eR;
	const vk::Bool32                    write_enable = VK_TRUE;
	vk::PipelineColorWriteCreateInfoEXT color_write {};
	color_write.attachmentCount    = 1;
	color_write.pColorWriteEnables = &write_enable;
	vk::PipelineColorBlendStateCreateInfo blend {};
	blend.pNext           = &color_write;
	blend.attachmentCount = 1;
	blend.pAttachments    = &blend_attachment;
	const std::array dynamic_states {vk::DynamicState::eViewportWithCount,
	                                 vk::DynamicState::eDepthBias};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	const vk::Format                color_format = vk::Format::eR8G8B8A8Unorm;
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &color_format;
	rendering.depthAttachmentFormat   = vk::Format::eD32Sfloat;
	vk::PipelineDepthStencilStateCreateInfo depth {};

	vk::GraphicsPipelineCreateInfo info {};
	info.pNext               = &rendering;
	info.stageCount          = 2;
	info.pStages             = stages;
	info.pVertexInputState   = &vertex;
	info.pInputAssemblyState = &assembly;
	info.pViewportState      = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState   = &multisample;
	info.pDepthStencilState  = &depth;
	info.pColorBlendState    = &blend;
	info.pDynamicState       = &dynamic;

	const PipelineRecord::LayoutState layout {{}, {}, {vk::ShaderStageFlagBits::eAllGraphics, 0, 128}};
	std::string error;
	const auto  json = PipelineRecord::EncodeGraphics(info, layout, Namer(), &error);
	Check(json.has_value(), "graphics pipeline encodes");
	auto decoded = PipelineRecord::Pipeline::Decode(*json, &error);
	Check(decoded.has_value() && !decoded->IsCompute() && decoded->Stages().size() == 2,
	      "graphics record decodes");
	const std::array modules {FakeModule(1), FakeModule(2)};
	const auto       r = decoded->BuildGraphics(modules, nullptr);
	const auto*      rebuilt_rendering = static_cast<const vk::PipelineRenderingCreateInfo*>(r.pNext);
	Check(rebuilt_rendering != nullptr && rebuilt_rendering->colorAttachmentCount == 1 &&
	          rebuilt_rendering->pColorAttachmentFormats[0] == color_format &&
	          rebuilt_rendering->depthAttachmentFormat == vk::Format::eD32Sfloat,
	      "rendering formats round-trip");
	Check(r.pVertexInputState->vertexAttributeDescriptionCount == 1 &&
	          r.pVertexInputState->pVertexAttributeDescriptions[0].offset == 4 &&
	          r.pVertexInputState->pVertexBindingDescriptions[0].stride == 16,
	      "vertex input round-trips");
	Check(r.pInputAssemblyState->topology == vk::PrimitiveTopology::eTriangleStrip,
	      "topology round-trips");
	Check(r.pViewportState->pNext != nullptr && r.pRasterizationState->pNext != nullptr &&
	          r.pRasterizationState->cullMode == vk::CullModeFlagBits::eBack,
	      "viewport and rasterization chains round-trip");
	Check(r.pMultisampleState->rasterizationSamples == vk::SampleCountFlagBits::e4,
	      "sample count round-trips");
	Check(r.pDepthStencilState != nullptr, "depth-stencil presence round-trips");
	Check(r.pColorBlendState->attachmentCount == 1 &&
	          r.pColorBlendState->pAttachments[0].srcColorBlendFactor ==
	              vk::BlendFactor::eSrc1Alpha &&
	          r.pColorBlendState->pNext != nullptr,
	      "blend state round-trips");
	Check(r.pDynamicState->dynamicStateCount == 2 &&
	          r.pDynamicState->pDynamicStates[1] == vk::DynamicState::eDepthBias,
	      "dynamic states round-trip");
	Check(r.pTessellationState == nullptr, "absent tessellation state stays absent");

	vk::PipelineRobustnessCreateInfoEXT unknown {};
	info.pNext = &unknown;
	Check(!PipelineRecord::EncodeGraphics(info, layout, Namer(), &error) && !error.empty(),
	      "unknown pNext structures are rejected instead of silently dropped");
}

void TestMalformedRecord() {
	std::string error;
	Check(!PipelineRecord::Pipeline::Decode(nlohmann::json::object(), &error) && !error.empty(),
	      "empty record is rejected");
	auto record = nlohmann::json::parse(
	    R"({"version":1,"kind":"compute","flags":0,"stages":[{"stage":"x"}],"layout":{}})", nullptr,
	    false);
	Check(!PipelineRecord::Pipeline::Decode(record, &error), "mistyped record is rejected");
}

} // namespace

int main() {
	TestQuirkDetection();
	TestQuirkOverrides();
	TestComputeRecord();
	TestGraphicsRecord();
	TestMalformedRecord();
	std::puts("ShaderCheckTests: ok");
	return 0;
}
