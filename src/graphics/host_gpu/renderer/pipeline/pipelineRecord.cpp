#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"

#include <fmt/format.h>

namespace Libs::Graphics::PipelineRecord {

namespace {

using nlohmann::json;

template <typename T>
uint32_t U(T value) {
	return static_cast<uint32_t>(value);
}

template <typename Flags>
uint32_t F(Flags flags) {
	return static_cast<uint32_t>(static_cast<typename Flags::MaskType>(flags));
}

bool Fail(std::string* error, std::string message) {
	if (error != nullptr) {
		*error = std::move(message);
	}
	return false;
}

// Visits every structure in a const pNext chain; returns false on the first one the callback
// does not accept.
template <typename Visit>
bool WalkChain(const void* next, std::string* error, const char* owner, Visit&& visit) {
	for (const auto* base = static_cast<const vk::BaseInStructure*>(next); base != nullptr;
	     base             = base->pNext) {
		if (!visit(*base)) {
			return Fail(error, fmt::format("{}: unsupported pNext structure {}", owner,
			                               vk::to_string(base->sType)));
		}
	}
	return true;
}

json EncodeLayout(const LayoutState& layout) {
	json bindings = json::array();
	for (const auto& b: layout.bindings) {
		if (b.pImmutableSamplers != nullptr) {
			// Immutable samplers are not used by the renderer; the checker could not recreate them.
			continue;
		}
		bindings.push_back({b.binding, U(b.descriptorType), b.descriptorCount, F(b.stageFlags)});
	}
	return {{"flags", F(layout.flags)},
	        {"bindings", std::move(bindings)},
	        {"push_constants",
	         {F(layout.push_constants.stageFlags), layout.push_constants.offset,
	          layout.push_constants.size}}};
}

std::optional<json> EncodeStage(const vk::PipelineShaderStageCreateInfo& stage,
                                const ModuleNamer& namer, std::string* error) {
	if (stage.pSpecializationInfo != nullptr && stage.pSpecializationInfo->mapEntryCount != 0) {
		Fail(error, "specialization constants are not recorded");
		return std::nullopt;
	}
	json out {{"stage", F(vk::ShaderStageFlags(stage.stage))},
	          {"entry", stage.pName != nullptr ? stage.pName : "main"}};
	const auto name = namer(stage.module);
	if (name.file.empty()) {
		Fail(error, fmt::format("{} module was not dumped", vk::to_string(stage.stage)));
		return std::nullopt;
	}
	out["spv"]        = name.file;
	out["guest_hash"] = fmt::format("{:016x}", name.guest_hash);
	if (F(stage.flags) != 0) {
		out["flags"] = F(stage.flags);
	}
	const bool ok = WalkChain(stage.pNext, error, "shader stage", [&](const auto& base) {
		if (base.sType == vk::StructureType::ePipelineShaderStageRequiredSubgroupSizeCreateInfo) {
			out["required_subgroup_size"] =
			    reinterpret_cast<const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo&>(base)
			        .requiredSubgroupSize;
			return true;
		}
		return false;
	});
	if (!ok) {
		return std::nullopt;
	}
	return out;
}

json EncodeBlendAttachment(const vk::PipelineColorBlendAttachmentState& a) {
	return {a.blendEnable,      U(a.srcColorBlendFactor), U(a.dstColorBlendFactor),
	        U(a.colorBlendOp),  U(a.srcAlphaBlendFactor), U(a.dstAlphaBlendFactor),
	        U(a.alphaBlendOp),  F(a.colorWriteMask)};
}

bool EncodeFixedFunction(const vk::GraphicsPipelineCreateInfo& info, json& out,
                         std::string* error) {
	if (const auto* vi = info.pVertexInputState; vi != nullptr) {
		if (vi->pNext != nullptr) {
			return Fail(error, "vertex input pNext is not recorded");
		}
		json bindings   = json::array();
		json attributes = json::array();
		for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
			const auto& b = vi->pVertexBindingDescriptions[i];
			bindings.push_back({b.binding, b.stride, U(b.inputRate)});
		}
		for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
			const auto& a = vi->pVertexAttributeDescriptions[i];
			attributes.push_back({a.location, a.binding, U(a.format), a.offset});
		}
		out["vertex_input"] = {{"bindings", std::move(bindings)},
		                       {"attributes", std::move(attributes)}};
	}
	if (const auto* ia = info.pInputAssemblyState; ia != nullptr) {
		out["input_assembly"] = {U(ia->topology), ia->primitiveRestartEnable};
	}
	if (const auto* ts = info.pTessellationState; ts != nullptr) {
		out["tessellation"] = ts->patchControlPoints;
	}
	if (const auto* vp = info.pViewportState; vp != nullptr) {
		if (vp->pViewports != nullptr || vp->pScissors != nullptr) {
			return Fail(error, "static viewports are not recorded");
		}
		json viewport {{"viewports", vp->viewportCount}, {"scissors", vp->scissorCount}};
		if (!WalkChain(vp->pNext, error, "viewport", [&](const auto& base) {
			    if (base.sType !=
			        vk::StructureType::ePipelineViewportDepthClipControlCreateInfoEXT) {
				    return false;
			    }
			    viewport["negative_one_to_one"] =
			        reinterpret_cast<const vk::PipelineViewportDepthClipControlCreateInfoEXT&>(base)
			            .negativeOneToOne;
			    return true;
		    })) {
			return false;
		}
		out["viewport"] = std::move(viewport);
	}
	if (const auto* rs = info.pRasterizationState; rs != nullptr) {
		json raster {{"depth_clamp", rs->depthClampEnable},
		             {"discard", rs->rasterizerDiscardEnable},
		             {"polygon_mode", U(rs->polygonMode)},
		             {"cull_mode", F(rs->cullMode)},
		             {"front_face", U(rs->frontFace)},
		             {"depth_bias", rs->depthBiasEnable},
		             {"depth_bias_values",
		              {rs->depthBiasConstantFactor, rs->depthBiasClamp, rs->depthBiasSlopeFactor}},
		             {"line_width", rs->lineWidth}};
		if (!WalkChain(rs->pNext, error, "rasterization", [&](const auto& base) {
			    switch (base.sType) {
				    case vk::StructureType::ePipelineRasterizationDepthClipStateCreateInfoEXT:
					    raster["depth_clip_enable"] = reinterpret_cast<
					        const vk::PipelineRasterizationDepthClipStateCreateInfoEXT&>(base)
					                                      .depthClipEnable;
					    return true;
				    case vk::StructureType::ePipelineRasterizationProvokingVertexStateCreateInfoEXT:
					    raster["provoking_vertex"] = U(
					        reinterpret_cast<
					            const vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT&>(base)
					            .provokingVertexMode);
					    return true;
				    default: return false;
			    }
		    })) {
			return false;
		}
		out["rasterization"] = std::move(raster);
	}
	if (const auto* ms = info.pMultisampleState; ms != nullptr) {
		if (ms->pNext != nullptr || ms->pSampleMask != nullptr) {
			return Fail(error, "multisample pNext/sample mask are not recorded");
		}
		out["multisample"] = {F(vk::SampleCountFlags(ms->rasterizationSamples)),
		                      ms->sampleShadingEnable, ms->minSampleShading,
		                      ms->alphaToCoverageEnable, ms->alphaToOneEnable};
	}
	if (const auto* ds = info.pDepthStencilState; ds != nullptr) {
		const auto stencil = [](const vk::StencilOpState& s) {
			return json {U(s.failOp),  U(s.passOp),    U(s.depthFailOp), U(s.compareOp),
			             s.compareMask, s.writeMask, s.reference};
		};
		out["depth_stencil"] = {{"depth", {ds->depthTestEnable, ds->depthWriteEnable,
		                                   U(ds->depthCompareOp), ds->depthBoundsTestEnable,
		                                   ds->minDepthBounds, ds->maxDepthBounds}},
		                        {"stencil_enable", ds->stencilTestEnable},
		                        {"front", stencil(ds->front)},
		                        {"back", stencil(ds->back)}};
	}
	if (const auto* cb = info.pColorBlendState; cb != nullptr) {
		json attachments = json::array();
		for (uint32_t i = 0; i < cb->attachmentCount; i++) {
			attachments.push_back(EncodeBlendAttachment(cb->pAttachments[i]));
		}
		json blend {{"logic_op", {cb->logicOpEnable, U(cb->logicOp)}},
		            {"attachments", std::move(attachments)},
		            {"constants",
		             {cb->blendConstants[0], cb->blendConstants[1], cb->blendConstants[2],
		              cb->blendConstants[3]}}};
		if (!WalkChain(cb->pNext, error, "color blend", [&](const auto& base) {
			    if (base.sType != vk::StructureType::ePipelineColorWriteCreateInfoEXT) {
				    return false;
			    }
			    const auto& cw = reinterpret_cast<const vk::PipelineColorWriteCreateInfoEXT&>(base);
			    json enables   = json::array();
			    for (uint32_t i = 0; i < cw.attachmentCount; i++) {
				    enables.push_back(cw.pColorWriteEnables[i]);
			    }
			    blend["color_write_enables"] = std::move(enables);
			    return true;
		    })) {
			return false;
		}
		out["color_blend"] = std::move(blend);
	}
	if (const auto* dyn = info.pDynamicState; dyn != nullptr) {
		json states = json::array();
		for (uint32_t i = 0; i < dyn->dynamicStateCount; i++) {
			states.push_back(U(dyn->pDynamicStates[i]));
		}
		out["dynamic_states"] = std::move(states);
	}
	return true;
}

} // namespace

// Exception-free accessors (the emulator builds with -fno-exceptions, where nlohmann aborts on a
// type mismatch). The first problem is kept; later reads return zero values.
class Reader {
public:
	const json& At(const json& j, const char* key) {
		if (j.is_object()) {
			if (const auto it = j.find(key); it != j.end()) {
				return *it;
			}
		}
		Error(fmt::format("missing '{}'", key));
		return Null();
	}
	const json& At(const json& j, int index) {
		if (j.is_array() && index >= 0 && static_cast<size_t>(index) < j.size()) {
			return j[static_cast<size_t>(index)];
		}
		Error(fmt::format("missing element {}", index));
		return Null();
	}
	const json& Array(const json& j, const char* key) {
		const auto& value = At(j, key);
		if (!value.is_array()) {
			Error(fmt::format("'{}' is not an array", key));
			return EmptyArray();
		}
		return value;
	}
	uint32_t U32(const json& j) {
		if (!j.is_number_integer() || j.get<int64_t>() < 0 ||
		    j.get<int64_t>() > static_cast<int64_t>(UINT32_MAX)) {
			Error("expected an unsigned 32-bit number");
			return 0;
		}
		return static_cast<uint32_t>(j.get<int64_t>());
	}
	float F32(const json& j) {
		if (!j.is_number()) {
			Error("expected a number");
			return 0.0f;
		}
		return j.get<float>();
	}
	std::string Str(const json& j) {
		if (!j.is_string()) {
			Error("expected a string");
			return {};
		}
		return j.get<std::string>();
	}
	template <typename T>
	T Enum(const json& j) {
		return static_cast<T>(U32(j));
	}
	template <typename Flags>
	Flags Mask(const json& j) {
		return Flags(static_cast<typename Flags::MaskType>(U32(j)));
	}
	[[nodiscard]] bool               Ok() const { return m_error.empty(); }
	[[nodiscard]] const std::string& Message() const { return m_error; }

private:
	void Error(std::string message) {
		if (m_error.empty()) {
			m_error = std::move(message);
		}
	}
	static const json& Null() {
		static const json value;
		return value;
	}
	static const json& EmptyArray() {
		static const json value = json::array();
		return value;
	}
	std::string m_error;
};

std::optional<json> EncodeCompute(const vk::ComputePipelineCreateInfo& info,
                                  const LayoutState& layout, const ModuleNamer& namer,
                                  std::string* error) {
	if (info.pNext != nullptr) {
		Fail(error, "compute pipeline pNext is not recorded");
		return std::nullopt;
	}
	auto stage = EncodeStage(info.stage, namer, error);
	if (!stage) {
		return std::nullopt;
	}
	return json {{"version", Version},
	             {"kind", "compute"},
	             {"flags", F(info.flags)},
	             {"stages", json::array({std::move(*stage)})},
	             {"layout", EncodeLayout(layout)}};
}

std::optional<json> EncodeGraphics(const vk::GraphicsPipelineCreateInfo& info,
                                   const LayoutState& layout, const ModuleNamer& namer,
                                   std::string* error) {
	json out {{"version", Version}, {"kind", "graphics"}, {"flags", F(info.flags)}};
	json stages = json::array();
	for (uint32_t i = 0; i < info.stageCount; i++) {
		auto stage = EncodeStage(info.pStages[i], namer, error);
		if (!stage) {
			return std::nullopt;
		}
		stages.push_back(std::move(*stage));
	}
	out["stages"] = std::move(stages);
	out["layout"] = EncodeLayout(layout);
	if (info.renderPass != nullptr) {
		Fail(error, "render passes are not recorded (the renderer uses dynamic rendering)");
		return std::nullopt;
	}
	const bool ok = WalkChain(info.pNext, error, "graphics pipeline", [&](const auto& base) {
		if (base.sType != vk::StructureType::ePipelineRenderingCreateInfo) {
			return false;
		}
		const auto& r = reinterpret_cast<const vk::PipelineRenderingCreateInfo&>(base);
		json colors   = json::array();
		for (uint32_t i = 0; i < r.colorAttachmentCount; i++) {
			colors.push_back(U(r.pColorAttachmentFormats[i]));
		}
		out["rendering"] = {{"view_mask", r.viewMask},
		                    {"colors", std::move(colors)},
		                    {"depth", U(r.depthAttachmentFormat)},
		                    {"stencil", U(r.stencilAttachmentFormat)}};
		return true;
	});
	if (!ok || !EncodeFixedFunction(info, out, error)) {
		return std::nullopt;
	}
	return out;
}

std::optional<Pipeline> Pipeline::Decode(const json& record, std::string* error) {
	Reader   r;
	Pipeline p;
	if (r.U32(r.At(record, "version")) != static_cast<uint32_t>(Version)) {
		Fail(error, "unsupported record version");
		return std::nullopt;
	}
	p.m_compute = r.Str(r.At(record, "kind")) == "compute";
	p.m_flags   = r.Mask<vk::PipelineCreateFlags>(r.At(record, "flags"));
	for (const auto& s: r.Array(record, "stages")) {
		Stage stage;
		stage.stage = r.Enum<vk::ShaderStageFlagBits>(r.At(s, "stage"));
		stage.spv   = r.Str(r.At(s, "spv"));
		if (s.contains("entry")) stage.entry = r.Str(r.At(s, "entry"));
		if (s.contains("required_subgroup_size")) {
			stage.required_subgroup_size = r.U32(r.At(s, "required_subgroup_size"));
		}
		if (s.contains("guest_hash")) {
			stage.guest_hash = std::strtoull(r.Str(r.At(s, "guest_hash")).c_str(), nullptr, 16);
		}
		p.m_stages.push_back(std::move(stage));
	}
	const auto& layout = r.At(record, "layout");
	p.m_layout_flags   = r.Mask<vk::DescriptorSetLayoutCreateFlags>(r.At(layout, "flags"));
	for (const auto& b: r.Array(layout, "bindings")) {
		p.m_bindings.push_back({r.U32(r.At(b, 0)), r.Enum<vk::DescriptorType>(r.At(b, 1)),
		                        r.U32(r.At(b, 2)), r.Mask<vk::ShaderStageFlags>(r.At(b, 3)),
		                        nullptr});
	}
	const auto& push = r.At(layout, "push_constants");
	p.m_push         = {r.Mask<vk::ShaderStageFlags>(r.At(push, 0)), r.U32(r.At(push, 1)),
	                    r.U32(r.At(push, 2))};
	if (!p.m_compute) {
		p.DecodeGraphics(record, r);
	}
	if (!r.Ok()) {
		Fail(error, "malformed record: " + r.Message());
		return std::nullopt;
	}
	return p;
}

void Pipeline::DecodeGraphics(const json& record, Reader& r) {
	if (record.contains("rendering")) {
		const auto& rendering = r.At(record, "rendering");
		for (const auto& c: r.Array(rendering, "colors")) {
			m_color_formats.push_back(r.Enum<vk::Format>(c));
		}
		vk::PipelineRenderingCreateInfo info {};
		info.viewMask                = r.U32(r.At(rendering, "view_mask"));
		info.depthAttachmentFormat   = r.Enum<vk::Format>(r.At(rendering, "depth"));
		info.stencilAttachmentFormat = r.Enum<vk::Format>(r.At(rendering, "stencil"));
		m_rendering                  = info;
	}
	if (record.contains("vertex_input")) {
		const auto& vi = r.At(record, "vertex_input");
		for (const auto& b: r.Array(vi, "bindings")) {
			m_vertex_bindings.push_back({r.U32(r.At(b, 0)), r.U32(r.At(b, 1)),
			                             r.Enum<vk::VertexInputRate>(r.At(b, 2))});
		}
		for (const auto& a: r.Array(vi, "attributes")) {
			m_vertex_attributes.push_back({r.U32(r.At(a, 0)), r.U32(r.At(a, 1)),
			                               r.Enum<vk::Format>(r.At(a, 2)), r.U32(r.At(a, 3))});
		}
		m_vertex_input = vk::PipelineVertexInputStateCreateInfo {};
	}
	if (record.contains("input_assembly")) {
		const auto&                              ia = r.At(record, "input_assembly");
		vk::PipelineInputAssemblyStateCreateInfo state {};
		state.topology               = r.Enum<vk::PrimitiveTopology>(r.At(ia, 0));
		state.primitiveRestartEnable = r.U32(r.At(ia, 1));
		m_input_assembly             = state;
	}
	if (record.contains("tessellation")) {
		vk::PipelineTessellationStateCreateInfo state {};
		state.patchControlPoints = r.U32(r.At(record, "tessellation"));
		m_tessellation           = state;
	}
	if (record.contains("viewport")) {
		const auto&                         vp = r.At(record, "viewport");
		vk::PipelineViewportStateCreateInfo state {};
		state.viewportCount = r.U32(r.At(vp, "viewports"));
		state.scissorCount  = r.U32(r.At(vp, "scissors"));
		m_viewport          = state;
		if (vp.contains("negative_one_to_one")) {
			vk::PipelineViewportDepthClipControlCreateInfoEXT clip {};
			clip.negativeOneToOne = r.U32(r.At(vp, "negative_one_to_one"));
			m_depth_clip_control  = clip;
		}
	}
	if (record.contains("rasterization")) {
		const auto&                              rs = r.At(record, "rasterization");
		vk::PipelineRasterizationStateCreateInfo state {};
		state.depthClampEnable        = r.U32(r.At(rs, "depth_clamp"));
		state.rasterizerDiscardEnable = r.U32(r.At(rs, "discard"));
		state.polygonMode             = r.Enum<vk::PolygonMode>(r.At(rs, "polygon_mode"));
		state.cullMode                = r.Mask<vk::CullModeFlags>(r.At(rs, "cull_mode"));
		state.frontFace               = r.Enum<vk::FrontFace>(r.At(rs, "front_face"));
		state.depthBiasEnable         = r.U32(r.At(rs, "depth_bias"));
		const auto& bias              = r.At(rs, "depth_bias_values");
		state.depthBiasConstantFactor = r.F32(r.At(bias, 0));
		state.depthBiasClamp          = r.F32(r.At(bias, 1));
		state.depthBiasSlopeFactor    = r.F32(r.At(bias, 2));
		state.lineWidth               = r.F32(r.At(rs, "line_width"));
		m_rasterization               = state;
		if (rs.contains("depth_clip_enable")) {
			vk::PipelineRasterizationDepthClipStateCreateInfoEXT clip {};
			clip.depthClipEnable = r.U32(r.At(rs, "depth_clip_enable"));
			m_depth_clip         = clip;
		}
		if (rs.contains("provoking_vertex")) {
			vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking {};
			provoking.provokingVertexMode =
			    r.Enum<vk::ProvokingVertexModeEXT>(r.At(rs, "provoking_vertex"));
			m_provoking = provoking;
		}
	}
	if (record.contains("multisample")) {
		const auto&                            ms = r.At(record, "multisample");
		vk::PipelineMultisampleStateCreateInfo state {};
		state.rasterizationSamples  = r.Enum<vk::SampleCountFlagBits>(r.At(ms, 0));
		state.sampleShadingEnable   = r.U32(r.At(ms, 1));
		state.minSampleShading      = r.F32(r.At(ms, 2));
		state.alphaToCoverageEnable = r.U32(r.At(ms, 3));
		state.alphaToOneEnable      = r.U32(r.At(ms, 4));
		m_multisample               = state;
	}
	if (record.contains("depth_stencil")) {
		const auto& ds      = r.At(record, "depth_stencil");
		const auto  stencil = [&r](const json& s) {
			vk::StencilOpState op {};
			op.failOp      = r.Enum<vk::StencilOp>(r.At(s, 0));
			op.passOp      = r.Enum<vk::StencilOp>(r.At(s, 1));
			op.depthFailOp = r.Enum<vk::StencilOp>(r.At(s, 2));
			op.compareOp   = r.Enum<vk::CompareOp>(r.At(s, 3));
			op.compareMask = r.U32(r.At(s, 4));
			op.writeMask   = r.U32(r.At(s, 5));
			op.reference   = r.U32(r.At(s, 6));
			return op;
		};
		const auto&                             depth = r.At(ds, "depth");
		vk::PipelineDepthStencilStateCreateInfo state {};
		state.depthTestEnable       = r.U32(r.At(depth, 0));
		state.depthWriteEnable      = r.U32(r.At(depth, 1));
		state.depthCompareOp        = r.Enum<vk::CompareOp>(r.At(depth, 2));
		state.depthBoundsTestEnable = r.U32(r.At(depth, 3));
		state.minDepthBounds        = r.F32(r.At(depth, 4));
		state.maxDepthBounds        = r.F32(r.At(depth, 5));
		state.stencilTestEnable     = r.U32(r.At(ds, "stencil_enable"));
		state.front                 = stencil(r.At(ds, "front"));
		state.back                  = stencil(r.At(ds, "back"));
		m_depth_stencil             = state;
	}
	if (record.contains("color_blend")) {
		const auto& cb = r.At(record, "color_blend");
		for (const auto& a: r.Array(cb, "attachments")) {
			vk::PipelineColorBlendAttachmentState state {};
			state.blendEnable         = r.U32(r.At(a, 0));
			state.srcColorBlendFactor = r.Enum<vk::BlendFactor>(r.At(a, 1));
			state.dstColorBlendFactor = r.Enum<vk::BlendFactor>(r.At(a, 2));
			state.colorBlendOp        = r.Enum<vk::BlendOp>(r.At(a, 3));
			state.srcAlphaBlendFactor = r.Enum<vk::BlendFactor>(r.At(a, 4));
			state.dstAlphaBlendFactor = r.Enum<vk::BlendFactor>(r.At(a, 5));
			state.alphaBlendOp        = r.Enum<vk::BlendOp>(r.At(a, 6));
			state.colorWriteMask      = r.Mask<vk::ColorComponentFlags>(r.At(a, 7));
			m_blend_attachments.push_back(state);
		}
		vk::PipelineColorBlendStateCreateInfo state {};
		const auto&                           logic = r.At(cb, "logic_op");
		state.logicOpEnable                         = r.U32(r.At(logic, 0));
		state.logicOp                               = r.Enum<vk::LogicOp>(r.At(logic, 1));
		const auto& constants                       = r.At(cb, "constants");
		for (uint32_t i = 0; i < 4; i++) {
			state.blendConstants[i] = r.F32(r.At(constants, static_cast<int>(i)));
		}
		m_color_blend = state;
		if (cb.contains("color_write_enables")) {
			std::vector<vk::Bool32> enables;
			for (const auto& e: r.Array(cb, "color_write_enables")) {
				enables.push_back(r.U32(e));
			}
			m_color_write_enables = std::move(enables);
		}
	}
	if (record.contains("dynamic_states")) {
		for (const auto& d: r.Array(record, "dynamic_states")) {
			m_dynamic_states.push_back(r.Enum<vk::DynamicState>(d));
		}
	}
}

vk::DescriptorSetLayoutCreateInfo Pipeline::LayoutInfo() const {
	vk::DescriptorSetLayoutCreateInfo info {};
	info.flags        = m_layout_flags;
	info.bindingCount = static_cast<uint32_t>(m_bindings.size());
	info.pBindings    = m_bindings.data();
	return info;
}

void Pipeline::BuildStages(std::span<const vk::ShaderModule> modules) {
	m_stage_infos.assign(m_stages.size(), {});
	m_subgroup_infos.assign(m_stages.size(), {});
	for (size_t i = 0; i < m_stages.size(); i++) {
		auto& info  = m_stage_infos[i];
		info.stage  = m_stages[i].stage;
		info.module = modules[i];
		info.pName  = m_stages[i].entry.c_str();
		if (m_stages[i].required_subgroup_size != 0) {
			m_subgroup_infos[i].requiredSubgroupSize = m_stages[i].required_subgroup_size;
			info.pNext                               = &m_subgroup_infos[i];
		}
	}
}

vk::ComputePipelineCreateInfo Pipeline::BuildCompute(vk::ShaderModule module,
                                                     vk::PipelineLayout layout) {
	BuildStages(std::span(&module, 1));
	vk::ComputePipelineCreateInfo info {};
	info.flags             = m_flags;
	info.stage             = m_stage_infos.front();
	info.layout            = layout;
	info.basePipelineIndex = -1;
	return info;
}

vk::GraphicsPipelineCreateInfo Pipeline::BuildGraphics(std::span<const vk::ShaderModule> modules,
                                                       vk::PipelineLayout                layout) {
	BuildStages(modules);
	vk::GraphicsPipelineCreateInfo info {};
	info.flags      = m_flags;
	info.stageCount = static_cast<uint32_t>(m_stage_infos.size());
	info.pStages    = m_stage_infos.data();
	info.layout     = layout;
	info.basePipelineIndex = -1;
	if (m_rendering) {
		m_rendering->colorAttachmentCount    = static_cast<uint32_t>(m_color_formats.size());
		m_rendering->pColorAttachmentFormats = m_color_formats.data();
		info.pNext                           = &*m_rendering;
	}
	if (m_vertex_input) {
		m_vertex_input->vertexBindingDescriptionCount = static_cast<uint32_t>(m_vertex_bindings.size());
		m_vertex_input->pVertexBindingDescriptions    = m_vertex_bindings.data();
		m_vertex_input->vertexAttributeDescriptionCount =
		    static_cast<uint32_t>(m_vertex_attributes.size());
		m_vertex_input->pVertexAttributeDescriptions = m_vertex_attributes.data();
		info.pVertexInputState                       = &*m_vertex_input;
	}
	info.pInputAssemblyState = m_input_assembly ? &*m_input_assembly : nullptr;
	info.pTessellationState  = m_tessellation ? &*m_tessellation : nullptr;
	if (m_viewport) {
		m_viewport->pNext   = m_depth_clip_control ? &*m_depth_clip_control : nullptr;
		info.pViewportState = &*m_viewport;
	}
	if (m_rasterization) {
		const void* chain = nullptr;
		if (m_depth_clip) {
			m_depth_clip->pNext = chain;
			chain               = &*m_depth_clip;
		}
		if (m_provoking) {
			m_provoking->pNext = chain;
			chain              = &*m_provoking;
		}
		m_rasterization->pNext   = chain;
		info.pRasterizationState = &*m_rasterization;
	}
	info.pMultisampleState  = m_multisample ? &*m_multisample : nullptr;
	info.pDepthStencilState = m_depth_stencil ? &*m_depth_stencil : nullptr;
	if (m_color_blend) {
		m_color_blend->attachmentCount = static_cast<uint32_t>(m_blend_attachments.size());
		m_color_blend->pAttachments    = m_blend_attachments.data();
		m_color_blend->pNext           = nullptr;
		if (m_color_write_enables) {
			m_color_write.attachmentCount    = static_cast<uint32_t>(m_color_write_enables->size());
			m_color_write.pColorWriteEnables = m_color_write_enables->data();
			m_color_blend->pNext             = &m_color_write;
		}
		info.pColorBlendState = &*m_color_blend;
	}
	if (!m_dynamic_states.empty()) {
		m_dynamic.dynamicStateCount = static_cast<uint32_t>(m_dynamic_states.size());
		m_dynamic.pDynamicStates    = m_dynamic_states.data();
		info.pDynamicState          = &m_dynamic;
	}
	return info;
}

} // namespace Libs::Graphics::PipelineRecord
