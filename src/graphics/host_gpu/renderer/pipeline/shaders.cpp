#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/perfStats.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/barycentricGeometryShader.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/meshVertexPullShader.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <fmt/format.h>

namespace Libs::Graphics {

// PPSA24156: serialize Vulkan pipeline compilation on Turnip V32. The driver
// previously crashed while 16/24-control-point tessellation pipelines and
// conventional graphics pipelines were compiling concurrently. Scope this
// workaround to the title, preserving parallelism for all other games.
static std::mutex g_gt_native_pipeline_mutex;

// Diagnostic-only TES replacement for the exact GT/A830/Turnip-v32 crash.
// Match the captured DS execution modes: Quads / FractionalEven / CW.
// Keep the original HS/TCS, VS, patchControlPoints and all fixed pipeline
// state. A constant gl_Position and an empty PS guarantee missing geometry,
// which is expected; this must never be enabled in a normal release.
static std::vector<uint32_t> MakeGtTurnipMinimalTessEval() {
	using ShaderRecompiler::Spirv::Builder;
	Builder b;
	b.RequireCapability(spv::CapabilityShader);
	b.RequireCapability(spv::CapabilityTessellation);
	b.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
	const auto void_type  = b.Type(spv::OpTypeVoid);
	const auto int_type   = b.Type(spv::OpTypeInt, 32u, 1u);
	const auto float_type = b.Type(spv::OpTypeFloat, 32u);
	const auto vec4_type  = b.Type(spv::OpTypeVector, float_type, 4u);
	const auto func_type  = b.Type(spv::OpTypeFunction, void_type);
	const auto per_vertex = b.DecoratedType(
	    spv::OpTypeStruct,
	    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
	     {spv::OpDecorate, {spv::DecorationBlock}}},
	    vec4_type);
	const auto out_struct_ptr = b.Type(spv::OpTypePointer, spv::StorageClassOutput, per_vertex);
	const auto out_vec4_ptr   = b.Type(spv::OpTypePointer, spv::StorageClassOutput, vec4_type);
	const auto gl_out = b.DefineGlobalVariable(out_struct_ptr, spv::StorageClassOutput);
	const auto idx0   = b.Constant(spv::OpConstant, int_type, 0u);
	const auto fzero  = b.Constant(spv::OpConstant, float_type, 0u);
	const auto fone   = b.Constant(spv::OpConstant, float_type, 0x3f800000u);
	const auto pos    = b.Constant(spv::OpConstantComposite, vec4_type, fzero, fzero, fzero, fone);
	const auto main   = b.AllocateId();
	b.AddExecutionMode(main, spv::ExecutionModeQuads);
	b.AddExecutionMode(main, spv::ExecutionModeSpacingFractionalEven);
	b.AddExecutionMode(main, spv::ExecutionModeVertexOrderCw);
	b.AddEntryPoint(spv::ExecutionModelTessellationEvaluation, main, "main", {gl_out});
	b.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone, func_type);
	b.AddFunction(spv::OpLabel, b.AllocateId());
	const auto address = b.AllocateId();
	b.AddFunction(spv::OpAccessChain, out_vec4_ptr, address, gl_out, idx0);
	b.AddFunction(spv::OpStore, address, pos);
	b.AddFunction(spv::OpReturn);
	b.AddFunction(spv::OpFunctionEnd);
	return b.Build();
}



// Third diagnostic stage: captured GT HS has OutputVertices=4 although the
// Vulkan input patchControlPoints is 24. Keep both distinct. TES and PS probes
// do not consume the original HS output, so only tessellation levels are
// needed. This replacement is NOT an image-preserving graphics workaround.
static std::vector<uint32_t> MakeGtTurnipMinimalTessControl(bool passthrough_location0 = false,
                                                              uint32_t packed_input_vertices = 0) {
	using ShaderRecompiler::Spirv::Builder;
	Builder b;
	b.RequireCapability(spv::CapabilityShader);
	b.RequireCapability(spv::CapabilityTessellation);
	b.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
	const auto void_type = b.Type(spv::OpTypeVoid);
	const auto bool_type = b.Type(spv::OpTypeBool);
	const auto int_type = b.Type(spv::OpTypeInt, 32u, 1u);
	const auto uint_type = b.Type(spv::OpTypeInt, 32u, 0u);
	const auto float_type = b.Type(spv::OpTypeFloat, 32u);
	const auto function_type = b.Type(spv::OpTypeFunction, void_type);
	const auto n4 = b.Constant(spv::OpConstant, uint_type, 4u);
	const auto n2 = b.Constant(spv::OpConstant, uint_type, 2u);
	const auto outer_array = b.Type(spv::OpTypeArray, float_type, n4);
	const auto inner_array = b.Type(spv::OpTypeArray, float_type, n2);
	const auto input_ptr = b.Type(spv::OpTypePointer, spv::StorageClassInput, int_type);
	const auto outer_ptr = b.Type(spv::OpTypePointer, spv::StorageClassOutput, outer_array);
	const auto inner_ptr = b.Type(spv::OpTypePointer, spv::StorageClassOutput, inner_array);
	const auto float_ptr = b.Type(spv::OpTypePointer, spv::StorageClassOutput, float_type);
	const auto invocation = b.DefineGlobalVariable(input_ptr, spv::StorageClassInput);
	const auto outer = b.DefineGlobalVariable(outer_ptr, spv::StorageClassOutput);
	const auto inner = b.DefineGlobalVariable(inner_ptr, spv::StorageClassOutput);
	b.AddAnnotation(spv::OpDecorate, invocation, spv::DecorationBuiltIn, spv::BuiltInInvocationId);
	b.AddAnnotation(spv::OpDecorate, outer, spv::DecorationBuiltIn, spv::BuiltInTessLevelOuter);
	b.AddAnnotation(spv::OpDecorate, outer, spv::DecorationPatch);
	b.AddAnnotation(spv::OpDecorate, inner, spv::DecorationBuiltIn, spv::BuiltInTessLevelInner);
	b.AddAnnotation(spv::OpDecorate, inner, spv::DecorationPatch);
	// Captured HS d9596b...: input Location 0 and output Location 0
	// both have exactly one 32-bit unsigned integer component. The
	// captured original TES reads this same Location 0 as uint. When
	// explicitly requested, forward that varying to recover the
	// original TES/FS interface without restoring the large HS program.
	// Keep default probe byte-for-byte unchanged when disabled.
	uint32_t location_input = 0, location_output = 0;
	std::vector<uint32_t> interfaces {invocation, outer, inner};
	// The captured GT HS 2f056d... and 30cc06... do not expose a scalar
	// Location 0. The LS passes [input_cp][2] uvec4, while their original
	// TES reads [4][12] uvec4. Keep the full interface with an inexpensive
	// packed passthrough instead of the empty TCS/TES/FS diagnostic trio.
	// This is an experimental geometry restore, not the original HS math.
	uint32_t packed_in_ptr = 0, packed_out_ptr = 0;
	uint32_t packed_in_vec4_ptr = 0, packed_out_vec4_ptr = 0;
	if (packed_input_vertices != 0) {
		const auto uvec4 = b.Type(spv::OpTypeVector, uint_type, 4u);
		const auto inner2 = b.Type(spv::OpTypeArray, uvec4, n2);
		const auto cp_count = b.Constant(spv::OpConstant, uint_type, packed_input_vertices);
		const auto in_array = b.Type(spv::OpTypeArray, inner2, cp_count);
		const auto n12 = b.Constant(spv::OpConstant, uint_type, 12u);
		const auto out_inner = b.Type(spv::OpTypeArray, uvec4, n12);
		const auto out_array = b.Type(spv::OpTypeArray, out_inner, n4);
		packed_in_ptr = b.DefineGlobalVariable(
		    b.Type(spv::OpTypePointer, spv::StorageClassInput, in_array),
		    spv::StorageClassInput);
		packed_out_ptr = b.DefineGlobalVariable(
		    b.Type(spv::OpTypePointer, spv::StorageClassOutput, out_array),
		    spv::StorageClassOutput);
		packed_in_vec4_ptr = b.Type(spv::OpTypePointer, spv::StorageClassInput, uvec4);
		packed_out_vec4_ptr = b.Type(spv::OpTypePointer, spv::StorageClassOutput, uvec4);
		b.AddAnnotation(spv::OpDecorate, packed_in_ptr, spv::DecorationLocation, 0u);
		b.AddAnnotation(spv::OpDecorate, packed_out_ptr, spv::DecorationLocation, 0u);
		interfaces.push_back(packed_in_ptr);
		interfaces.push_back(packed_out_ptr);
	}
	if (passthrough_location0) {
		const auto in_uint_ptr = b.Type(spv::OpTypePointer, spv::StorageClassInput, uint_type);
		const auto out_uint_ptr = b.Type(spv::OpTypePointer, spv::StorageClassOutput, uint_type);
		location_input = b.DefineGlobalVariable(in_uint_ptr, spv::StorageClassInput);
		location_output = b.DefineGlobalVariable(out_uint_ptr, spv::StorageClassOutput);
		b.AddAnnotation(spv::OpDecorate, location_input, spv::DecorationLocation, 0u);
		b.AddAnnotation(spv::OpDecorate, location_output, spv::DecorationLocation, 0u);
		interfaces.push_back(location_input);
		interfaces.push_back(location_output);
	}
	const auto zero = b.Constant(spv::OpConstant, int_type, 0u);
	const auto one = b.Constant(spv::OpConstant, float_type, 0x3f800000u);
	const auto main = b.AllocateId();
	b.AddExecutionMode(main, spv::ExecutionModeOutputVertices, 4u);
	b.AddEntryPoint(spv::ExecutionModelTessellationControl, main, "main", interfaces);
	b.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone, function_type);
	b.AddFunction(spv::OpLabel, b.AllocateId());
	if (passthrough_location0) {
		const auto value = b.AllocateId();
		b.AddFunction(spv::OpLoad, uint_type, value, location_input);
		b.AddFunction(spv::OpStore, location_output, value);
	}
	const auto id = b.AllocateId();
	b.AddFunction(spv::OpLoad, int_type, id, invocation);
	if (packed_input_vertices != 0) {
		const auto six = b.Constant(spv::OpConstant, int_type, 6u);
		const auto cp = b.Constant(spv::OpConstant, int_type, packed_input_vertices);
		const auto base = b.AllocateId();
		b.AddFunction(spv::OpIMul, int_type, base, id, six);
		const auto uvec4 = b.Type(spv::OpTypeVector, uint_type, 4u);
		for (uint32_t group = 0; group < 6u; ++group) {
			const auto delta = b.Constant(spv::OpConstant, int_type, group);
			const auto src_linear = b.AllocateId();
			b.AddFunction(spv::OpIAdd, int_type, src_linear, base, delta);
			// The 16-cp HS generates 48 output vectors from 32 source
			// vectors. Wrap the remaining indices; 28-cp captures use 24
			// vertices directly. Never index an input past its declared size.
			const auto source = b.AllocateId();
			b.AddFunction(spv::OpSMod, int_type, source, src_linear, cp);
			for (uint32_t component = 0; component < 2u; ++component) {
				const auto in_component = b.Constant(spv::OpConstant, int_type, component);
				const auto out_component = b.Constant(
				    spv::OpConstant, int_type, group * 2u + component);
				const auto in_address = b.AllocateId();
				b.AddFunction(spv::OpAccessChain, packed_in_vec4_ptr,
				              in_address, packed_in_ptr, source, in_component);
				const auto value = b.AllocateId();
				b.AddFunction(spv::OpLoad, uvec4, value, in_address);
				const auto out_address = b.AllocateId();
				b.AddFunction(spv::OpAccessChain, packed_out_vec4_ptr,
				              out_address, packed_out_ptr, id, out_component);
				b.AddFunction(spv::OpStore, out_address, value);
			}
		}
	}
	const auto first = b.AllocateId();
	b.AddFunction(spv::OpIEqual, bool_type, first, id, zero);
	const auto write = b.AllocateId();
	const auto merge = b.AllocateId();
	b.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
	b.AddFunction(spv::OpBranchConditional, first, write, merge);
	b.AddFunction(spv::OpLabel, write);
	for (uint32_t i = 0; i < 6u; i++) {
		const auto index = b.Constant(spv::OpConstant, int_type, i < 4u ? i : i - 4u);
		const auto ptr = b.AllocateId();
		b.AddFunction(spv::OpAccessChain, float_ptr, ptr, i < 4u ? outer : inner, index);
		b.AddFunction(spv::OpStore, ptr, one);
	}
	b.AddFunction(spv::OpBranch, merge);
	b.AddFunction(spv::OpLabel, merge);
	b.AddFunction(spv::OpReturn);
	b.AddFunction(spv::OpFunctionEnd);
	return b.Build();
}

// IDK: maybe we can remove it?
constexpr uint8_t kTemporaryVertexAttribFormat113 =
    static_cast<uint8_t>(Prospero::VertexAttribFormat::k16_16SInt);
constexpr uint32_t kTemporaryPs5BufferFormat121 = 121u;
// PS5 format 52 is a true 64-bit unsigned vertex attribute (GT reference PR #1033).
constexpr uint32_t kVertexBufferFormat64xU64 = 52u;

static bool NarrowInputFormat(vk::Format& format, uint32_t& size, uint32_t used_components) {
	if (used_components == 0 || used_components >= size) {
		return false;
	}

	switch (format) {
		case vk::Format::eR32G32B32A32Sfloat:
			switch (used_components) {
				case 1: format = vk::Format::eR32Sfloat; break;
				case 2: format = vk::Format::eR32G32Sfloat; break;
				case 3: format = vk::Format::eR32G32B32Sfloat; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR32G32B32Sfloat:
			switch (used_components) {
				case 1: format = vk::Format::eR32Sfloat; break;
				case 2: format = vk::Format::eR32G32Sfloat; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR16G16B16A16Sfloat:
			switch (used_components) {
				case 1: format = vk::Format::eR16Sfloat; break;
				case 2: format = vk::Format::eR16G16Sfloat; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR8G8B8A8Unorm:
			switch (used_components) {
				case 1: format = vk::Format::eR8Unorm; break;
				case 2: format = vk::Format::eR8G8Unorm; break;
				default: return false;
			}
			size = used_components;
			return true;
		case vk::Format::eR8G8B8A8Snorm:
			if (used_components != 2) {
				return false;
			}
			format = vk::Format::eR8G8Snorm;
			size   = 2;
			return true;
		case vk::Format::eR8G8B8A8Uint:
			switch (used_components) {
				case 1: format = vk::Format::eR8Uint; break;
				case 2: format = vk::Format::eR8G8Uint; break;
				default: return false;
			}
			size = used_components;
			return true;
		default: break;
	}

	return false;
}

static void GetInputFormat(const ShaderBufferResource& res, vk::Format& format, uint32_t& size,
                           uint32_t used_components) {
	const auto fmt        = res.Format();
	const auto raw_format = res.RawFormat();
	if (raw_format == kTemporaryVertexAttribFormat113) {
		static bool logged_113 = false;
		if (!logged_113) {
			LOGF("InputFormat: temporary: accepting invalid PS5 buffer format 113 as "
			     "vk::Format::eR32G32B32A32Sfloat\n");
			logged_113 = true;
		}
		format = vk::Format::eR32G32B32A32Sfloat;
		size   = 4;
		if (NarrowInputFormat(format, size, used_components)) {
			LOGF("InputFormat: narrowing fmt=%u to %s for used_components=%u\n", raw_format,
			     vk::to_string(format).c_str(), used_components);
		}
		return;
	}
	if (raw_format == kTemporaryPs5BufferFormat121) {
		static bool logged_121 = false;
		if (!logged_121) {
			LOGF("InputFormat: accepting PS5 buffer format 121 as vk::Format::eR16G16Sfloat\n");
			logged_121 = true;
		}
		format = vk::Format::eR16G16Sfloat;
		size   = 2;
		return;
	}

	if (raw_format == kVertexBufferFormat64xU64) {
		// A single 64-bit guest vertex datum is fetched as two raw dwords.
		// The guest IR carries them as separate U32 components, *not* an
		// OpTypeInt(64) vertex input. Adreno/Turnip does not expose R64Uint
		// as a vertex-fetch format; R32G32Uint preserves all 64 bits in the
		// exact same 8 bytes without numerical conversion to FP32.
		// The shader can declare uvec3/uvec4 even when the source format
		// contains only two 32-bit components. Vulkan fills missing unsigned
		// components with 0 (and the fourth with 1). Do NOT fetch 12 or 16 bytes:
		// source format 52 contains only the original 8 bytes.
		if (used_components == 0 || used_components > 4) {
			EXIT("Invalid PS5 vertex format 52 component count %u\n", used_components);
		}
		format = used_components == 1 ? vk::Format::eR32Uint : vk::Format::eR32G32Uint;
		size   = 8;
		static std::atomic_bool logged_52 {false};
		if (!logged_52.exchange(true, std::memory_order_relaxed))
			LOGF("VertexInput: PS5 U64 attribute represented by two U32 halves (format=%u)\n",
			     static_cast<uint32_t>(format));
		return;
	}

	format = VulkanFormat(fmt);
	size   = ShaderRecompiler::Format::GetFormatInfo(fmt).component_count;
	if (format == vk::Format::eUndefined || size == 0) {
		EXIT("unknown vertex format: fmt = %u\n", raw_format);
	}

	if (NarrowInputFormat(format, size, used_components)) {
		static std::atomic<uint64_t> log_count = 0;
		auto                         log_id    = log_count.fetch_add(1);
		if (log_id < 32) {
			LOGF("VertexInput: narrowed vertex format to %" PRIu32
			     " component(s) for shader fetch\n",
			     used_components);
		}
	}
}

static vk::BlendFactor GetBlendFactor(uint32_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kZero: return vk::BlendFactor::eZero;
		case Prospero::BlendFactor::kOne: return vk::BlendFactor::eOne;
		case Prospero::BlendFactor::kSrcColor: return vk::BlendFactor::eSrcColor;
		case Prospero::BlendFactor::kOneMinusSrcColor: return vk::BlendFactor::eOneMinusSrcColor;
		case Prospero::BlendFactor::kSrcAlpha: return vk::BlendFactor::eSrcAlpha;
		case Prospero::BlendFactor::kOneMinusSrcAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
		case Prospero::BlendFactor::kDstAlpha: return vk::BlendFactor::eDstAlpha;
		case Prospero::BlendFactor::kOneMinusDstAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
		case Prospero::BlendFactor::kDstColor: return vk::BlendFactor::eDstColor;
		case Prospero::BlendFactor::kOneMinusDstColor: return vk::BlendFactor::eOneMinusDstColor;
		case Prospero::BlendFactor::kSrcAlphaSaturate: return vk::BlendFactor::eSrcAlphaSaturate;
		case Prospero::BlendFactor::kConstantColor: return vk::BlendFactor::eConstantColor;
		case Prospero::BlendFactor::kOneMinusConstantColor:
			return vk::BlendFactor::eOneMinusConstantColor;
		case Prospero::BlendFactor::kSrc1Color: return vk::BlendFactor::eSrc1Color;
		case Prospero::BlendFactor::kOneMinusSrc1Color: return vk::BlendFactor::eOneMinusSrc1Color;
		case Prospero::BlendFactor::kSrc1Alpha: return vk::BlendFactor::eSrc1Alpha;
		case Prospero::BlendFactor::kOneMinusSrc1Alpha: return vk::BlendFactor::eOneMinusSrc1Alpha;
		case Prospero::BlendFactor::kConstantAlpha: return vk::BlendFactor::eConstantAlpha;
		case Prospero::BlendFactor::kOneMinusConstantAlpha:
			return vk::BlendFactor::eOneMinusConstantAlpha;
		default: EXIT("unknown factor: %u\n", factor);
	}
	return vk::BlendFactor::eZero;
}

static vk::BlendOp GetBlendOp(uint32_t op) {
	switch (static_cast<Prospero::BlendOp>(op)) {
		case Prospero::BlendOp::kAdd: return vk::BlendOp::eAdd;
		case Prospero::BlendOp::kSubtract: return vk::BlendOp::eSubtract;
		case Prospero::BlendOp::kMin: return vk::BlendOp::eMin;
		case Prospero::BlendOp::kMax: return vk::BlendOp::eMax;
		case Prospero::BlendOp::kReverseSubtract: return vk::BlendOp::eReverseSubtract;
		default: EXIT("unknown op: %u\n", op);
	}
	return vk::BlendOp::eAdd;
}

static void AddLayoutBindings(std::vector<vk::DescriptorSetLayoutBinding>& descriptor_bindings,
                              const ShaderRecompiler::IR::CompiledShaderInfo& program,
                              vk::ShaderStageFlagBits              stage) {
	for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
		const auto index = std::countr_zero(mask);
		const auto kind = static_cast<ShaderRecompiler::IR::DescriptorBindingKind>(index);
		descriptor_bindings.push_back(
		    {ShaderRecompiler::IR::NativeBinding(program.stage, kind), NativeDescriptorType(kind),
		     program.bindings.descriptor_counts[index], stage, nullptr});
	}
}

static void CreateDescriptorLayout(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                   std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	uint64_t descriptor_count = 0;
	uint64_t storage_buffers = 0;
	uint64_t uniform_buffers = 0;
	std::array<uint64_t, 32> stage_buffers {}, stage_uniforms {}, stage_resources {};
	for (const auto& binding: bindings) {
		descriptor_count += binding.descriptorCount;
		const bool storage = binding.descriptorType == vk::DescriptorType::eStorageBuffer;
		const bool uniform = binding.descriptorType == vk::DescriptorType::eUniformBuffer;
		if (storage) storage_buffers += binding.descriptorCount;
		if (uniform) uniform_buffers += binding.descriptorCount;
		for (auto stages = static_cast<uint32_t>(binding.stageFlags); stages != 0; stages &= stages - 1u) {
			const auto stage = std::countr_zero(stages);
			if (storage) stage_buffers[stage] += binding.descriptorCount;
			if (uniform) stage_uniforms[stage] += binding.descriptorCount;
			if (binding.descriptorType != vk::DescriptorType::eSampler)
				stage_resources[stage] += binding.descriptorCount;
			if (stage_buffers[stage] > limits.maxPerStageDescriptorStorageBuffers ||
			    stage_uniforms[stage] > limits.maxPerStageDescriptorUniformBuffers ||
			    stage_resources[stage] > limits.maxPerStageResources) {
				EXIT("native descriptor stage %u exceeds device limits: buffers=%" PRIu64
				     "/%u uniform buffers=%" PRIu64 "/%u resources=%" PRIu64 "/%u\n", stage,
				     stage_buffers[stage], limits.maxPerStageDescriptorStorageBuffers,
				     stage_uniforms[stage], limits.maxPerStageDescriptorUniformBuffers,
				     stage_resources[stage], limits.maxPerStageResources);
			}
		}
	}
	if (storage_buffers > limits.maxDescriptorSetStorageBuffers ||
	    uniform_buffers > limits.maxDescriptorSetUniformBuffers) {
		EXIT("native buffer descriptors exceed device limits: storage=%" PRIu64 "/%u uniform=%" PRIu64 "/%u\n",
		     storage_buffers, limits.maxDescriptorSetStorageBuffers, uniform_buffers,
		     limits.maxDescriptorSetUniformBuffers);
	}
	pipeline.uses_push_descriptors = descriptor_count <= graphics.max_push_descriptors;

	vk::DescriptorSetLayoutCreateInfo create {};
	create.flags        = pipeline.uses_push_descriptors
	                          ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                          : vk::DescriptorSetLayoutCreateFlags {};
	create.bindingCount = static_cast<uint32_t>(bindings.size());
	create.pBindings    = bindings.data();
	EXIT_IF(graphics.device.createDescriptorSetLayout(
	            &create, nullptr, &pipeline.descriptor_set_layout) != vk::Result::eSuccess);
}

static BarycentricPrimitive BarycentricPrimitiveFor(vk::PrimitiveTopology topology) {
	switch (topology) {
		case vk::PrimitiveTopology::ePointList: return BarycentricPrimitive::Points;
		case vk::PrimitiveTopology::eLineList:
		case vk::PrimitiveTopology::eLineStrip: return BarycentricPrimitive::Lines;
		case vk::PrimitiveTopology::eTriangleList:
		case vk::PrimitiveTopology::eTriangleStrip:
		case vk::PrimitiveTopology::eTriangleFan: return BarycentricPrimitive::Triangles;
		default:
			EXIT("emulated fragment barycentrics: topology %s has no geometry-stage input "
			     "primitive\n",
			     vk::to_string(topology).c_str());
	}
	return BarycentricPrimitive::Triangles;
}

// A pixel shader whose barycentrics are emulated (no VK_KHR_fragment_shader_barycentric) reads
// them from a generated pass-through geometry stage. The stage is a pure function of the pixel
// shader, the last pre-rasterization stage and the topology, which all key the pipeline.
// Returns null when the pixel shader needs no such stage.
static vk::ShaderModule CreateBarycentricGeometryModule(
    GraphicContext& graphics, std::span<const ShaderVertexInputInfo> vertex_info,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology, bool rect_list,
    bool mesh, const MeshVertexPullStage* mesh_vertex_pull) {
	if (ps_input_info == nullptr) {
		return nullptr;
	}
	const auto& pixel_program = *ps_input_info->stage.program;
	const auto& emulation     = pixel_program.info.barycentric_emulation;
	if (!emulation.enabled) {
		return nullptr;
	}
	if (mesh && mesh_vertex_pull == nullptr) {
		EXIT("pixel shader 0x%016" PRIx64 " needs emulated fragment barycentrics, which need a "
		     "geometry stage, and a mesh pipeline cannot have one\n",
		     pixel_program.shader_hash);
	}
	BarycentricGeometryStage stage {
	    .fragment   = &emulation,
	    .point_size = graphics.optional_features.geometry_point_size,
	};
	if (rect_list) {
		// The rect-list evaluation stage emits triangles and forwards only the parameters.
		stage.primitive          = BarycentricPrimitive::Triangles;
		stage.previous_locations = RectListParameterLocations(vertex_info.front(), ps_input_info);
	} else if (mesh_vertex_pull != nullptr) {
		// An emulated mesh stage draws triangles through the generated vertex stage.
		stage.primitive          = BarycentricPrimitive::Triangles;
		stage.previous           = MeshVertexPullOutputs(*mesh_vertex_pull);
		stage.previous_locations = MeshVertexPullParameterMask(*mesh_vertex_pull);
	} else {
		const auto& previous     = *vertex_info.back().stage.program;
		stage.primitive          = vertex_info.size() == 3 ? BarycentricPrimitive::Triangles
		                                                   : BarycentricPrimitiveFor(topology);
		stage.previous           = previous.info.raster_outputs;
		stage.previous_locations = previous.param_export_mask;
	}
	const auto& limits     = graphics.physical_device_properties.limits;
	const auto  components = BarycentricGeometryOutputComponents(stage);
	const auto  vertices   = stage.primitive == BarycentricPrimitive::Triangles ? 3u
	                         : stage.primitive == BarycentricPrimitive::Lines   ? 2u
	                                                                            : 1u;
	if (components > limits.maxGeometryOutputComponents ||
	    components * vertices > limits.maxGeometryTotalOutputComponents) {
		EXIT("pixel shader 0x%016" PRIx64 " needs %u varying components per vertex for emulated "
		     "fragment barycentrics; the device's geometry stage allows %u (%u in total)\n",
		     pixel_program.shader_hash, components, limits.maxGeometryOutputComponents,
		     limits.maxGeometryTotalOutputComponents);
	}
	const auto code   = BuildBarycentricGeometryShader(stage);
	if (graphics_debug_dump_enabled()) {
		static std::atomic_uint32_t s_dumps = 0;
		const auto path = Config::GetShaderLogFolder() /
		                  fmt::format("{:04d}_barycentric_gs_{:016x}.spv", s_dumps++,
		                              pixel_program.shader_hash);
		Common::File::CreateDirectories(path.parent_path());
		Common::File file(path);
		if (!file.IsInvalid()) {
			file.Write(code.data(), code.size() * sizeof(uint32_t));
		}
	}
	const auto module = CompileSPV(code, graphics.device);
	static std::atomic_uint32_t s_pipelines = 0;
	LOGF("Barycentric GS: pipeline #%u PS=0x%016" PRIx64 " primitive=%u varyings=%zu "
	     "components=%u words=%zu\n",
	     ++s_pipelines, pixel_program.shader_hash, static_cast<uint32_t>(stage.primitive),
	     emulation.varyings.size(), components, code.size());
	return module;
}

static vk::ShaderModule CreateMeshVertexPullModule(
    GraphicContext& graphics, const ShaderRecompiler::IR::CompiledShaderInfo& mesh_program,
    const MeshVertexPullStage& stage) {
	const auto code = BuildMeshVertexPullShader(stage);
	if (graphics_debug_dump_enabled()) {
		static std::atomic_uint32_t s_dumps = 0;
		const auto path = Config::GetShaderLogFolder() /
		                  fmt::format("{:04d}_mesh_vertex_pull_{:016x}.spv", s_dumps++,
		                              mesh_program.shader_hash);
		Common::File::CreateDirectories(path.parent_path());
		Common::File file(path);
		if (!file.IsInvalid()) {
			file.Write(code.data(), code.size() * sizeof(uint32_t));
		}
	}
	static std::atomic_uint32_t s_pipelines = 0;
	LOGF("Mesh emulation: vertex stage #%u MS=0x%016" PRIx64 " stride=%u dwords fields=%zu "
	     "words=%zu\n",
	     ++s_pipelines, mesh_program.shader_hash, stage.layout->stride_dwords,
	     stage.layout->fields.size(), code.size());
	return CompileSPV(code, graphics.device);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
vk::Result CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache                      driver_cache) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	const bool  tessellation   = vertex_info.size() == 3;
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(!vertex_program || (ps_active && !pixel_program));
	const bool with_depth = rendering.depth_format != vk::Format::eUndefined ||
	                        rendering.stencil_format != vk::Format::eUndefined;
	EXIT_IF(!vs_input_info.stage);
	const bool mesh = vs_input_info.stage.program->stage == ShaderType::Mesh;
	// Without VK_EXT_mesh_shader the mesh stage ran as compute; a generated vertex stage reads
	// its vertex records back for an ordinary indexed triangle-list draw.
	const bool mesh_native   = mesh && !vs_input_info.stage.program->info.mesh_emulation;
	const bool mesh_emulated = mesh && !mesh_native;
	EXIT_IF(mesh_native && !graphics.mesh_shader_enabled);
	EXIT_IF(mesh_emulated && static_params.topology != vk::PrimitiveTopology::eTriangleList);
	const bool rect_list =
	    !mesh && !tessellation && static_params.topology == vk::PrimitiveTopology::ePatchList;
	std::optional<ShaderRecompiler::IR::MeshEmulationLayout> mesh_layout;
	std::optional<MeshVertexPullStage>                       mesh_vertex_pull;
	vk::ShaderModule                                         mesh_vertex_module = nullptr;
	if (mesh_emulated) {
		mesh_layout = ShaderRecompiler::IR::BuildMeshEmulationLayout(
		    std::span<const ShaderRecompiler::IR::StageOutput>(
		        vs_input_info.stage.program->info.outputs));
		static const bool sanitize = [] {
			const char* value = std::getenv("KYTY_MESH_CULL_BAD");
			return value == nullptr || std::strcmp(value, "0") != 0;
		}();
		static const bool count = [] {
			const char* value = std::getenv("KYTY_MESH_POS_COUNT");
			return value != nullptr && std::strcmp(value, "1") == 0;
		}();
		mesh_vertex_pull = MeshVertexPullStage {
		    .layout = &*mesh_layout, .sanitize_positions = sanitize, .count_positions = count};
		const auto& features = graphics.optional_features;
		if (!features.fragment_shader_barycentric && features.geometry_varying_locations != 0) {
			// The same location vertex shaders mirror layer/viewport into for that stage.
			mesh_vertex_pull->layer_viewport_location = features.geometry_varying_locations - 1u;
			const auto location = mesh_vertex_pull->layer_viewport_location;
			if (location >= 32u || (MeshVertexPullParameterMask(*mesh_vertex_pull) &
			                        (1u << location)) != 0) {
				// Parameters own that location; the geometry stage then sees layer 0.
				mesh_vertex_pull->layer_viewport_location = UINT32_MAX;
			}
		}
		mesh_vertex_module = CreateMeshVertexPullModule(graphics, *vs_input_info.stage.program,
		                                                *mesh_vertex_pull);
	}

	vk::ShaderModule tess_control_shader_module = nullptr;
	vk::ShaderModule tess_eval_shader_module    = nullptr;

	if (rect_list) {
		const auto shaders =
		    BuildRectListShaders(vs_input_info, ps_active ? ps_input_info : nullptr);
		tess_control_shader_module = CompileSPV(shaders.control, graphics.device);
		ShaderDump::RecordModule(tess_control_shader_module, "rect_tcs", 0, shaders.control);
		if (graphics_debug_dump_enabled()) {
			LOGF("PipelineTrace: vkCreateShaderModule RectList TCS done module=%p\n",
			     static_cast<void*>(tess_control_shader_module));
		}

		tess_eval_shader_module = CompileSPV(shaders.evaluation, graphics.device);
		ShaderDump::RecordModule(tess_eval_shader_module, "rect_tes", 0, shaders.evaluation);
		if (graphics_debug_dump_enabled()) {
			LOGF("PipelineTrace: vkCreateShaderModule RectList TES done module=%p\n",
			     static_cast<void*>(tess_eval_shader_module));
		}
	}

	EXIT_NOT_IMPLEMENTED(
	    rect_list && (tess_control_shader_module == nullptr || tess_eval_shader_module == nullptr));

	const auto geometry_shader_module = CreateBarycentricGeometryModule(
	    graphics, vertex_info, ps_active ? ps_input_info : nullptr, static_params.topology,
	    rect_list, mesh, mesh_vertex_pull ? &*mesh_vertex_pull : nullptr);

	vk::PipelineShaderStageCreateInfo shader_stages[5] {};
	uint32_t                          shader_stage_count = 0;
	if (mesh_emulated) {
		shader_stages[shader_stage_count++] = {.stage  = vk::ShaderStageFlagBits::eVertex,
		                                       .module = mesh_vertex_module,
		                                       .pName  = "main"};
	} else {
		for (uint32_t i = 0; i < vertex_info.size(); i++) {
			shader_stages[shader_stage_count++] = {
			    .stage  = NativeShaderStage(vertex_info[i].logical_stage),
			    .module = programs.vertex[i].module,
			    .pName  = "main"};
		}
	}
	if (rect_list) {
		shader_stages[shader_stage_count++] = {.stage =
		                                           vk::ShaderStageFlagBits::eTessellationControl,
		                                       .module = tess_control_shader_module,
		                                       .pName  = "main"};
		shader_stages[shader_stage_count++] = {.stage =
		                                           vk::ShaderStageFlagBits::eTessellationEvaluation,
		                                       .module = tess_eval_shader_module,
		                                       .pName  = "main"};
	}
	if (geometry_shader_module != nullptr) {
		shader_stages[shader_stage_count++] = {.stage  = vk::ShaderStageFlagBits::eGeometry,
		                                       .module = geometry_shader_module,
		                                       .pName  = "main"};
	}
	if (ps_active) {
		shader_stages[shader_stage_count++] = {.stage  = vk::ShaderStageFlagBits::eFragment,
		                                       .module = pixel_program.module,
		                                       .pName  = "main"};
	}

	// Only the captured HS d9596b... exposes an unsigned 32-bit
	// input and output at Location 0, consumed by the original TES.
	// This is opt-in, retains the proven three-minimal-stage fallback
	// unless enabled, and does not affect other guest HS signatures.
	const char* gt_restore_option = std::getenv("KYTY_GT_TURNIP_VISUAL_RESTORE");
	const uint64_t gt_hs_hash = vertex_info.size() > 1
	    ? vertex_info[1].stage.program->shader_hash : 0;
	// Shader shapes verified against the two actual SPIR-V bundles exported
	// by GT at 17:12 and 17:15. The first HS input is [28][2] uvec4 and
	// the second is [16][2] uvec4; both TES inputs are [4][12] uvec4.
	// Never apply the packed pass to another unverified shader signature.
	const uint32_t gt_packed_input_vertices =
	    gt_hs_hash == 0x2f056d60c0ea5193ULL ? 28u :
	    gt_hs_hash == 0x30cc064c6fc120d4ULL ? 16u : 0u;
	const bool gt_scalar_restore = gt_hs_hash == 0xd9596b2d7a2e4202ULL;
	const bool gt_visual_restore_requested =
	    tessellation && ps_active && vertex_info.size() > 1 &&
	    gt_restore_option != nullptr && std::strcmp(gt_restore_option, "1") == 0 &&
	    (gt_scalar_restore || gt_packed_input_vertices != 0);

	// Stage-isolation experiment for the documented Turnip V32 SIGSEGV on
	// PPSA24156 / A830. Keep VS/TCS/TES and every other pipeline parameter
	// unchanged, but use a valid empty fragment shader for tessellated draws.
	// If the ICD still crashes, the original fragment module is unnecessary.
	// This intentionally makes tessellated geometry visually incomplete;
	// it is NOT a production workaround. Disable via
	// KYTY_GT_TURNIP_FRAGMENT_PROBE=0 to compare with the original behavior.
	vk::ShaderModule gt_fragment_probe_module = nullptr;
	if (tessellation && ps_active && !gt_visual_restore_requested) {
		const auto& props = graphics.GetPhysicalDeviceProperties();
		std::string title;
		const bool gt = Loader::SystemContentParamSfoGetString("TITLE_ID", &title) &&
		                title == "PPSA24156";
		const char* toggle = std::getenv("KYTY_GT_TURNIP_FRAGMENT_PROBE");
		if (gt && (toggle == nullptr || std::strcmp(toggle, "0") != 0) &&
		    props.vendorID == 20803u && props.deviceID == 1141178369u &&
		    props.driverVersion == 109060195u) {
			const std::vector<uint32_t> empty_fragment {
			    0x07230203u, 0x00010300u, 0u, 6u, 0u,
			    0x00020011u, 1u,
			    0x0003000eu, 0u, 1u,
			    0x0005000fu, 4u, 4u, 0x6e69616du, 0u,
			    0x00030010u, 4u, 7u,
			    0x00020013u, 1u,
			    0x00030021u, 2u, 1u,
			    0x00050036u, 1u, 4u, 0u, 2u,
			    0x000200f8u, 5u,
			    0x000100fdu,
			    0x00010038u,
			};
			gt_fragment_probe_module = CompileSPV(empty_fragment, graphics.device);
			EXIT_IF(gt_fragment_probe_module == nullptr);
			ShaderDump::RecordModule(gt_fragment_probe_module, "turnip_probe_fs", 0,
			                         empty_fragment);
			shader_stages[shader_stage_count - 1].module = gt_fragment_probe_module;
			static std::atomic_uint32_t once {0};
			if (once.fetch_add(1, std::memory_order_relaxed) < 8) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "GT TURNIP PROBE: using empty fragment stage; HS={:016x} original_PS={:016x}\n",
				    vertex_info[1].stage.program->shader_hash,
				    ps_input_info->stage.program->shader_hash));
			}
		}
	}


	// After the empty-fragment experiment reproduced the *same* ICD null
	// dereference at pipeline #69, isolate the tessellation evaluation stage.
	// The control/hull shader stays original; the evaluation stage emits only
	// the required built-in position. This is explicitly not a rendering fix.
	vk::ShaderModule gt_tes_probe_module = nullptr;
	if (tessellation && ps_active && !gt_visual_restore_requested) {
		const auto& props = graphics.GetPhysicalDeviceProperties();
		std::string title;
		const char* toggle = std::getenv("KYTY_GT_TURNIP_TES_PROBE");
		if ((toggle == nullptr || std::strcmp(toggle, "0") != 0) &&
		    Loader::SystemContentParamSfoGetString("TITLE_ID", &title) &&
		    title == "PPSA24156" && props.vendorID == 20803u &&
		    props.deviceID == 1141178369u && props.driverVersion == 109060195u) {
			const auto minimal_tes = MakeGtTurnipMinimalTessEval();
			gt_tes_probe_module = CompileSPV(minimal_tes, graphics.device);
			EXIT_IF(gt_tes_probe_module == nullptr);
			ShaderDump::RecordModule(gt_tes_probe_module, "turnip_probe_tes", 0, minimal_tes);
			for (uint32_t i = 0; i < shader_stage_count; i++) {
				if (shader_stages[i].stage == vk::ShaderStageFlagBits::eTessellationEvaluation) {
					shader_stages[i].module = gt_tes_probe_module;
					break;
				}
			}
			static std::atomic_uint32_t once {0};
			if (once.fetch_add(1, std::memory_order_relaxed) < 8) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "GT TURNIP TES PROBE: minimal DS/TES active; original HS={:016x} (unchanged) "
				    "original PS replaced={}\n",
				    vertex_info[1].stage.program->shader_hash,
				    gt_fragment_probe_module != nullptr ? 1 : 0));
			}
		}
	}


	// Isolate remaining native HS/TCS after the FS and TES probes still
	// crashed inside the same Turnip ICD function. Default on ONLY on
	// PPSA24156 + the captured A830 Turnip v32. To A/B compare use
	// KYTY_GT_TURNIP_TCS_PROBE=0.
	vk::ShaderModule gt_tcs_probe_module = nullptr;
	// The known-good minimal control shader is paired with minimal TES/FS.
	// The opt-in Location-0 passthrough is the only exception: reflection
	// of the captured guest HS/DS confirms the original uint interface.
	if (tessellation && ps_active &&
	    (gt_visual_restore_requested ||
	     (gt_tes_probe_module != nullptr && gt_fragment_probe_module != nullptr))) {
		const auto& props = graphics.GetPhysicalDeviceProperties();
		std::string title;
		const char* toggle = std::getenv("KYTY_GT_TURNIP_TCS_PROBE");
		if ((toggle == nullptr || std::strcmp(toggle, "0") != 0) &&
		    Loader::SystemContentParamSfoGetString("TITLE_ID", &title) &&
		    title == "PPSA24156" && props.vendorID == 20803u &&
		    props.deviceID == 1141178369u && props.driverVersion == 109060195u) {
			const auto minimal_tcs = MakeGtTurnipMinimalTessControl(
			    gt_visual_restore_requested && gt_scalar_restore,
			    gt_visual_restore_requested ? gt_packed_input_vertices : 0);
			gt_tcs_probe_module = CompileSPV(minimal_tcs, graphics.device);
			EXIT_IF(gt_tcs_probe_module == nullptr);
			ShaderDump::RecordModule(gt_tcs_probe_module, "turnip_probe_tcs", 0, minimal_tcs);
			bool replaced = false;
			for (uint32_t i = 0; i < shader_stage_count; i++) {
				if (shader_stages[i].stage == vk::ShaderStageFlagBits::eTessellationControl) {
					shader_stages[i].module = gt_tcs_probe_module;
					replaced = true;
					break;
				}
			}
			EXIT_IF(!replaced);
			static std::atomic_uint32_t once {0};
			if (once.fetch_add(1, std::memory_order_relaxed) < 12u) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "GT TURNIP TCS PROBE: minimal HS/TCS active, output_vertices=4 "
				    "input_patch_control_points={} original_HS={:016x} "
				    "passthrough_location0={} packed_input_vertices={} TES_minimal={} FS_minimal={}\n",
				    vs_input_info.tess.input_control_points,
				    vertex_info[1].stage.program->shader_hash,
				    gt_visual_restore_requested && gt_scalar_restore ? 1 : 0,
				    gt_visual_restore_requested ? gt_packed_input_vertices : 0u,
				    gt_tes_probe_module != nullptr ? 1 : 0,
				    gt_fragment_probe_module != nullptr ? 1 : 0));
			}
		}
	}

	vk::VertexInputAttributeDescription input_attr[ShaderVertexInputInfo::RES_MAX] {};
	vk::VertexInputBindingDescription   input_desc[ShaderVertexInputInfo::RES_MAX] {};

	for (uint32_t binding = 0; binding < vertex_input.binding_count; binding++) {
		input_desc[binding].binding   = binding;
		input_desc[binding].stride    = vertex_input.bindings[binding].stride;
		input_desc[binding].inputRate = vertex_input.bindings[binding].instance
		                                    ? vk::VertexInputRate::eInstance
		                                    : vk::VertexInputRate::eVertex;
	}
	for (uint32_t index = 0; index < vertex_input.attribute_count; index++) {
		input_attr[index].binding  = vertex_input.attributes[index].binding;
		input_attr[index].location = index;
		input_attr[index].offset   = vertex_input.attributes[index].offset;

		uint32_t   attr_size     = 4;
		const auto registers_num = vs_input_info.resources_dst[index].registers_num;
		const auto compiled_components =
		    vs_input_info.stage.program->info.vertex_fetch_components[index];
		const auto used_components =
		    compiled_components > 0 ? static_cast<int>(compiled_components) : registers_num;
		GetInputFormat(vs_input_info.resources[index], input_attr[index].format, attr_size,
		               static_cast<uint32_t>(used_components));
		// Vulkan VUID-VkVertexInputAttributeDescription-format-00623:
		// report an unsupported vertex-fetch format before passing it to the ICD.
		if (!(graphics.GetFormatProperties(input_attr[index].format).bufferFeatures &
		      vk::FormatFeatureFlagBits::eVertexBuffer)) {
			EXIT("Pipeline vertex attribute %u unsupported by this Vulkan GPU: format=%u\n",
			     index, static_cast<uint32_t>(input_attr[index].format));
		}

		if (graphics_debug_dump_enabled()) {
			static std::atomic_uint log_count = 0;
			const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
			if (log_id < 128) {
				LOGF("VertexInputState[%u]: attr=%u binding=%u offset=%u stride=%u fmt=%d "
				     "src_fmt=%u dst=v%u regs=%u"
				     " fetched_components=%u attr_size=%u swizzle=%u,%u,%u,%u\n",
				     log_id, index, input_attr[index].binding, input_attr[index].offset,
				     input_desc[input_attr[index].binding].stride,
				     static_cast<int>(input_attr[index].format),
				     static_cast<uint32_t>(vs_input_info.resources[index].Format()),
				     static_cast<uint32_t>(vs_input_info.resources_dst[index].register_start),
				     static_cast<uint32_t>(registers_num), static_cast<uint32_t>(used_components),
				     attr_size, static_cast<uint32_t>(vs_input_info.resources[index].DstSelX()),
				     static_cast<uint32_t>(vs_input_info.resources[index].DstSelY()),
				     static_cast<uint32_t>(vs_input_info.resources[index].DstSelZ()),
				     static_cast<uint32_t>(vs_input_info.resources[index].DstSelW()));
			}
		}

		if (vs_input_info.resources[index].OutOfBounds() != 0) {
			static bool logged = false;
			if (!logged) {
				LOGF("VertexInput: temporary: accepting PS5 out-of-bounds behavior %" PRIu8 "\n",
				     vs_input_info.resources[index].OutOfBounds());
				logged = true;
			}
		}

		EXIT_IF(registers_num < 1 || registers_num > 4);
	}

	vk::PipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.vertexBindingDescriptionCount   = vertex_input.binding_count;
	vertex_input_info.pVertexBindingDescriptions      = input_desc;
	vertex_input_info.vertexAttributeDescriptionCount = vertex_input.attribute_count;
	vertex_input_info.pVertexAttributeDescriptions    = input_attr;

	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = static_params.topology;
	input_assembly.primitiveRestartEnable =
	    static_params.primitive_restart_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.negativeOneToOne = (static_params.negative_one_to_one ? VK_TRUE : VK_FALSE);

	vk::PipelineViewportStateCreateInfo viewport_state {};
	viewport_state.pNext = &depth_clip_control;

	vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eNone;
	if (static_params.cull_back) {
		cull_mode |= vk::CullModeFlagBits::eBack;
	}
	if (static_params.cull_front) {
		cull_mode |= vk::CullModeFlagBits::eFront;
	}

	vk::FrontFace front_face =
	    (static_params.face ? vk::FrontFace::eClockwise : vk::FrontFace::eCounterClockwise);

	vk::PipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.depthClipEnable = static_params.depth_clip_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineRasterizationStateCreateInfo rasterizer {};
	// The DB clamps depth to the viewport range after polygon offset is applied. MoltenVK does
	// not enable depthClamp, so it keeps Vulkan's default clipping.
#if !defined(__APPLE__)
	rasterizer.depthClampEnable = VK_TRUE;
#endif
	// Without VK_EXT_depth_clip_enable the struct is omitted, which leaves Vulkan's default depth
	// clipping in place. Where depthClampEnable is set, that clamp already covers the PS5 behavior.
	if (graphics.optional_features.depth_clip_enable) {
		rasterizer.pNext = &clip_ext;
	} else if (!static_params.depth_clip_enable) {
		static std::atomic_bool s_warned = false;
		if (!s_warned.exchange(true)) {
			LOGF("WARNING: depth clip disable requested but VK_EXT_depth_clip_enable is "
			     "unsupported; per-pipeline depth clip control is not applied\n");
		}
	}
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex {};
	EXIT_NOT_IMPLEMENTED(static_params.provoking_vtx_last &&
	                     !graphics.provoking_vertex_last_enabled);
	if (graphics.provoking_vertex_last_enabled) {
		provoking_vertex.provokingVertexMode = static_params.provoking_vtx_last
		    ? vk::ProvokingVertexModeEXT::eLastVertex : vk::ProvokingVertexModeEXT::eFirstVertex;
		provoking_vertex.pNext = rasterizer.pNext;
		rasterizer.pNext = &provoking_vertex;
	}
	rasterizer.cullMode  = cull_mode;
	rasterizer.frontFace = front_face;
	rasterizer.polygonMode = static_params.polygon_mode;
	rasterizer.lineWidth = 1.0f;

	vk::PipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sampleShadingEnable  = static_params.sample_shading_enable ? VK_TRUE : VK_FALSE;
	multisampling.rasterizationSamples = vulkan_sample_count(static_params.samples);
	multisampling.minSampleShading     = 1.0f;
	multisampling.alphaToCoverageEnable =
	    static_params.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE;

	vk::PipelineColorBlendAttachmentState color_blend_attachment[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < rendering.color_count; i++) {
		EXIT_NOT_IMPLEMENTED((static_params.color_mask[i] & ~0x0fu) != 0);
		color_blend_attachment[i].colorWriteMask =
		    vk::ColorComponentFlags {static_params.color_mask[i]};
		color_blend_attachment[i].blendEnable = static_params.blend_enable[i] ? VK_TRUE : VK_FALSE;
		color_blend_attachment[i].srcColorBlendFactor =
		    GetBlendFactor(static_params.color_srcblend[i]);
		color_blend_attachment[i].dstColorBlendFactor =
		    GetBlendFactor(static_params.color_destblend[i]);
		color_blend_attachment[i].colorBlendOp = GetBlendOp(static_params.color_comb_fcn[i]);
		color_blend_attachment[i].srcAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i]
		         ? GetBlendFactor(static_params.alpha_srcblend[i])
		         : color_blend_attachment[i].srcColorBlendFactor);
		color_blend_attachment[i].dstAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i]
		         ? GetBlendFactor(static_params.alpha_destblend[i])
		         : color_blend_attachment[i].dstColorBlendFactor);
		color_blend_attachment[i].alphaBlendOp =
		    (static_params.separate_alpha_blend[i] ? GetBlendOp(static_params.alpha_comb_fcn[i])
		                                           : color_blend_attachment[i].colorBlendOp);
	}

	vk::Bool32 color_write_enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < rendering.color_count; i++) {
		color_write_enable[i] = VK_TRUE;
	}

	vk::PipelineColorWriteCreateInfoEXT color_write {};
	color_write.attachmentCount    = rendering.color_count;
	color_write.pColorWriteEnables = color_write_enable;

	vk::PipelineColorBlendStateCreateInfo color_blending {};
	// Without VK_EXT_color_write_enable the struct is omitted. Each attachment then uses only its
	// static guest colorWriteMask; attachments without an image view are not written anyway.
	if (graphics.optional_features.color_write_enable) {
		color_blending.pNext = &color_write;
	}
	color_blending.logicOp         = vk::LogicOp::eCopy;
	color_blending.attachmentCount = rendering.color_count;
	color_blending.pAttachments    = color_blend_attachment;

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	vk::ShaderStageFlags graphics_stages = vk::ShaderStageFlagBits::eFragment;
	if (mesh_emulated) {
		// The mesh program's resources belong to its compute pipeline; the generated vertex
		// stage reads only push data.
		graphics_stages |= vk::ShaderStageFlagBits::eVertex;
	} else {
		for (const auto& stage: vertex_info) {
			const auto native_stage = NativeShaderStage(stage.logical_stage);
			AddLayoutBindings(descriptor_bindings, *stage.stage.program, native_stage);
			graphics_stages |= native_stage;
		}
	}
	if (ps_active) {
		EXIT_IF(!ps_input_info->stage);
		AddLayoutBindings(descriptor_bindings, *ps_input_info->stage.program,
		                  vk::ShaderStageFlagBits::eFragment);
	}
	CreateDescriptorLayout(graphics, pipeline, descriptor_bindings);
	const vk::PushConstantRange push_constants {graphics_stages, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};
	pipeline.push_constant_stages = graphics_stages;

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &pipeline.descriptor_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_IF(pipeline.pipeline_layout != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout begin VS=%" PRIu64 " PS=%" PRIu64
		     " set_layouts=1 push_constants=%" PRIu32 "\n",
		     vertex_program.id, ps_active ? pixel_program.id : 0, 1u);
	}
	auto result = graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                   &pipeline.pipeline_layout);
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreatePipelineLayout done result=%s layout=%p\n",
		     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline_layout));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);

	vk::PipelineDepthStencilStateCreateInfo depth_stencil_info {};

	std::vector<vk::DynamicState> dynamic_states {
	    vk::DynamicState::eViewportWithCount,
	    vk::DynamicState::eScissorWithCount,
	    vk::DynamicState::eLineWidth,
	    vk::DynamicState::eDepthTestEnable,
	    vk::DynamicState::eDepthWriteEnable,
	    vk::DynamicState::eDepthCompareOp,
	    vk::DynamicState::eDepthBiasEnable,
	    vk::DynamicState::eDepthBias,
	    vk::DynamicState::eStencilTestEnable,
	    vk::DynamicState::eStencilOp,
	    vk::DynamicState::eStencilCompareMask,
	    vk::DynamicState::eStencilReference,
	    vk::DynamicState::eStencilWriteMask,
	    vk::DynamicState::eBlendConstants,
	};
	if (graphics.optional_features.depth_bounds) {
		dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
		dynamic_states.push_back(vk::DynamicState::eDepthBounds);
	}
	if (rendering.color_count != 0 && graphics.optional_features.color_write_enable) {
		dynamic_states.push_back(vk::DynamicState::eColorWriteEnableEXT);
	}
	if (graphics.attachment_feedback_loop_enabled) {
		dynamic_states.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
	}

	vk::PipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic_state.pDynamicStates    = dynamic_states.data();

	vk::GraphicsPipelineCreateInfo  pipeline_info {};
	vk::PipelineRenderingCreateInfo rendering_info {};
	rendering_info.colorAttachmentCount    = rendering.color_count;
	rendering_info.pColorAttachmentFormats = rendering.color_formats.data();
	rendering_info.depthAttachmentFormat   = rendering.depth_format;
	rendering_info.stencilAttachmentFormat = rendering.stencil_format;
	pipeline_info.pNext                    = &rendering_info;
	pipeline_info.stageCount               = shader_stage_count;
	pipeline_info.pStages                  = shader_stages;
	pipeline_info.pVertexInputState        = mesh_native ? nullptr : &vertex_input_info;
	pipeline_info.pInputAssemblyState      = mesh_native ? nullptr : &input_assembly;
	vk::PipelineTessellationStateCreateInfo tessellation_state {};
	tessellation_state.patchControlPoints =
	    tessellation ? vs_input_info.tess.input_control_points : 3u;
	pipeline_info.pTessellationState = (rect_list || tessellation) ? &tessellation_state : nullptr;
	pipeline_info.pViewportState          = &viewport_state;
	pipeline_info.pRasterizationState     = &rasterizer;
	pipeline_info.pMultisampleState       = &multisampling;
	pipeline_info.pDepthStencilState      = (with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState        = &color_blending;
	pipeline_info.pDynamicState           = &dynamic_state;
	pipeline_info.layout                  = pipeline.pipeline_layout;
	pipeline_info.basePipelineIndex       = -1;

	EXIT_IF(pipeline.pipeline != nullptr);

	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines begin VS=%" PRIu64 " PS=%" PRIu64
		     " topology=%" PRIu32 " color_mask=0x%08" PRIx32
		     " depth=%s blend=%s dyn_states=%" PRIu32 "\n",
		     vertex_program.id, ps_active ? pixel_program.id : 0,
		     static_cast<uint32_t>(static_params.topology), static_params.color_mask[0],
		     (with_depth ? "true" : "false"), (static_params.blend_enable[0] ? "true" : "false"),
		     dynamic_state.dynamicStateCount);
	}
	std::array<uint64_t, 4> guest_hashes {};
	uint32_t                guest_hash_count = 0;
	for (const auto& stage: vertex_info) {
		guest_hashes[guest_hash_count++] = stage.stage.program->shader_hash;
	}
	if (ps_active) {
		guest_hashes[guest_hash_count++] = ps_input_info->stage.program->shader_hash;
	}
	const bool capture_stats =
	    PipelineStats::Wants(graphics, std::span(guest_hashes).first(guest_hash_count));
	if (capture_stats) {
		pipeline_info.flags |=
		    PipelineStats::CaptureFlags(std::span(guest_hashes).first(guest_hash_count));
	}
	const auto dump_record = ShaderDump::RecordGraphicsPipeline(
	    graphics, pipeline_info,
	    {descriptor_bindings, pipeline.uses_push_descriptors
	                              ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                              : vk::DescriptorSetLayoutCreateFlags {},
	     push_constants},
	    std::span(guest_hashes).first(guest_hash_count));
	// An incomplete set of native tessellation modules must never be passed to a
	// Vulkan ICD: failed compilation can leave the second or third stage empty.
	// Validate every stage, not just the first VS and the optional PS.
	for (uint32_t i = 0; i < shader_stage_count; ++i) {
		if (shader_stages[i].module == nullptr) {
			EXIT("graphics pipeline missing shader module: stage=%u/%u stage_bits=%u\n",
			     i, shader_stage_count, static_cast<uint32_t>(shader_stages[i].stage));
		}
	}
	// The PPSA24156 crash in Turnip V32 is inside vkCreateGraphicsPipelines,
	// before the VkResult comes back. Record a *paired* per-pipeline breadcrumb
	// so the crash report identifies exactly which native stages and attachment
	// combination was in flight (there can be three shader workers).
	std::string title_id;
	const bool gt_pipeline = Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) &&
	                         title_id == "PPSA24156";
	static std::atomic_uint64_t gt_pipeline_seq {0};
	const auto gt_serial = gt_pipeline ? gt_pipeline_seq.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
	if (gt_pipeline) {
		std::string stages;
		for (uint32_t i = 0; i < shader_stage_count; ++i) {
			stages += fmt::format(" [{}:{} module={}]", i,
			    static_cast<uint32_t>(shader_stages[i].stage),
			    reinterpret_cast<uintptr_t>(static_cast<VkShaderModule>(shader_stages[i].module)));
		}
		std::string guest_shader_hashes;
		for (uint32_t i = 0; i < guest_hash_count; ++i)
			guest_shader_hashes += fmt::format(" 0x{:016x}", guest_hashes[i]);
		std::string colors;
		for (uint32_t i = 0; i < rendering.color_count; ++i)
			colors += fmt::format(" {}:{}", i, static_cast<uint32_t>(rendering.color_formats[i]));
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT pipeline BEGIN #{} VS=0x{:016x} PS=0x{:016x} hashes={} "
		    "topology={} tess={} cp={} stages={} raster_samples={} "
		    "color_count={} color_formats={} depth={} stencil={} cache={}\n",
		    gt_serial, vs_input_info.stage.program->shader_hash,
		    ps_active ? ps_input_info->stage.program->shader_hash : 0ull,
		    guest_shader_hashes,
		    static_cast<uint32_t>(static_params.topology), tessellation ? 1 : 0,
		    tessellation_state.patchControlPoints, stages,
		    static_params.samples, rendering.color_count, colors,
		    static_cast<uint32_t>(rendering.depth_format),
		    static_cast<uint32_t>(rendering.stencil_format),
		    driver_cache != nullptr ? 1 : 0));
	}
	// All three unfinished pipelines in the 181836 device crash have
	// native tessellation with 16/24 control points. The concurrent shader
	// workers were inside the same Turnip V32 compiler when it dereferenced
	// 0x10. This compatibility gate affects ONLY PPSA24156: serialize those
	// expensive tessellation compilations, and bypass the driver's cache for
	// them while leaving the cache intact for ordinary graphics pipelines.
	// This is a narrow workaround candidate, not proof of a Mesa driver bug.
	const bool gt_heavy_tess = gt_pipeline && tessellation &&
	                           tessellation_state.patchControlPoints >= 16;
	const bool gt_nocache_requested =
	    gt_pipeline && std::getenv("KYTY_GT_NO_PIPELINE_CACHE") != nullptr &&
	    std::strcmp(std::getenv("KYTY_GT_NO_PIPELINE_CACHE"), "1") == 0;
	const bool gt_nocache = gt_heavy_tess || gt_nocache_requested;
	if (gt_pipeline && tessellation &&
	    tessellation_state.patchControlPoints >
	        graphics.GetPhysicalDeviceProperties().limits.maxTessellationPatchSize) {
		EXIT("GT tessellation patch control points %u exceed Vulkan limit %u\n",
		     tessellation_state.patchControlPoints,
		     graphics.GetPhysicalDeviceProperties().limits.maxTessellationPatchSize);
	}
	if (gt_heavy_tess)
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT tess pipeline QUEUE #{} cp={} cache_bypass=1\n",
		    gt_serial, tessellation_state.patchControlPoints));
	{
		std::unique_lock<std::mutex> serialize(
		    g_gt_native_pipeline_mutex, std::defer_lock);
		if (gt_pipeline)
			serialize.lock();
		if (gt_heavy_tess)
			Log::WriteToConsoleAndLog(fmt::format(
			    "GT tess pipeline ENTER #{}\n", gt_serial));
		const Common::PerfStats::ScopedStall timing(
		    ShaderWorkerPool::OnWorkerThread() ? Common::PerfStats::StallKind::PipelineWorker
		                                       : Common::PerfStats::StallKind::PipelineCompile);
		result = graphics.device.createGraphicsPipelines(
		    gt_nocache ? vk::PipelineCache {} : driver_cache, 1,
		    &pipeline_info, nullptr, &pipeline.pipeline);
		if (gt_heavy_tess)
			Log::WriteToConsoleAndLog(fmt::format(
			    "GT tess pipeline EXIT #{} result={}\n",
			    gt_serial, vk::to_string(result)));
	}
	if (gt_pipeline) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT pipeline END #{} result={} cache_bypassed={}\n",
		    gt_serial, vk::to_string(result), gt_nocache ? 1 : 0));
	}
	if (gt_fragment_probe_module != nullptr) {
		ShaderDump::ForgetModule(gt_fragment_probe_module);
		graphics.device.destroyShaderModule(gt_fragment_probe_module, nullptr);
	}
	if (gt_tes_probe_module != nullptr) {
		ShaderDump::ForgetModule(gt_tes_probe_module);
		graphics.device.destroyShaderModule(gt_tes_probe_module, nullptr);
	}
	if (gt_tcs_probe_module != nullptr) {
		ShaderDump::ForgetModule(gt_tcs_probe_module);
		graphics.device.destroyShaderModule(gt_tcs_probe_module, nullptr);
	}
	ShaderDump::RecordResult(dump_record, result);
	if (capture_stats && result == vk::Result::eSuccess) {
		std::string label = "gfx";
		for (uint32_t i = 0; i < guest_hash_count; i++) {
			label += fmt::format("_{:016x}", guest_hashes[i]);
		}
		PipelineStats::Write(graphics, pipeline.pipeline, label,
		                     std::span(guest_hashes).first(guest_hash_count));
	}
	auto release_temporary_modules = [&] {
		if (tess_control_shader_module != nullptr) {
			ShaderDump::ForgetModule(tess_control_shader_module);
			graphics.device.destroyShaderModule(tess_control_shader_module, nullptr);
		}
		if (tess_eval_shader_module != nullptr) {
			ShaderDump::ForgetModule(tess_eval_shader_module);
			graphics.device.destroyShaderModule(tess_eval_shader_module, nullptr);
		}
		if (geometry_shader_module != nullptr)
			graphics.device.destroyShaderModule(geometry_shader_module, nullptr);
		if (mesh_vertex_module != nullptr)
			graphics.device.destroyShaderModule(mesh_vertex_module, nullptr);
	};
	// Workers cannot retire renderer-owned pipelines or submit its command buffer.
	// Hand the actual GT allocation error back to the recording thread instead.
	if (gt_pipeline && (result == vk::Result::eErrorOutOfHostMemory ||
	                    result == vk::Result::eErrorOutOfDeviceMemory)) {
		release_temporary_modules();
		return result;
	}
	if (result != vk::Result::eSuccess) {
		std::string stages;
		for (uint32_t i = 0; i < guest_hash_count; i++) {
			stages += fmt::format(" 0x{:016x}", guest_hashes[i]);
		}
		EXIT("vkCreateGraphicsPipelines failed: %s, guest shaders:%s%s%s\n",
		     vk::to_string(result).c_str(), stages.c_str(),
		     dump_record.empty() ? "" : ", dump record: ", dump_record.c_str());
	}
	if (graphics_debug_dump_enabled()) {
		LOGF("PipelineTrace: vkCreateGraphicsPipelines done result=%s pipeline=%p\n",
		     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline));
	}
	if (result != vk::Result::eSuccess && mesh_emulated) {
		EXIT("vkCreateGraphicsPipelines failed: %s for emulated mesh stage 0x%016" PRIx64
		     " PS=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), vs_input_info.stage.program->shader_hash,
		     pixel_program.id);
	}
	if (result != vk::Result::eSuccess && geometry_shader_module != nullptr) {
		EXIT("vkCreateGraphicsPipelines failed: %s for VS=%" PRIu64 " PS=%" PRIu64
		     " with an emulated-barycentric geometry stage\n",
		     vk::to_string(result).c_str(), vertex_program.id, pixel_program.id);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);

	release_temporary_modules();
	return vk::Result::eSuccess;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderRecompiler::IR::CompiledShaderInfo& program,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache) {
	EXIT_IF(compute_module == nullptr);

	vk::PipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.stage  = vk::ShaderStageFlagBits::eCompute;
	comp_shader_stage_info.module = compute_module;
	comp_shader_stage_info.pName  = "main";
	const auto wave_size = program.wave_size;
	if (graphics.compute_subgroup_size_control_enabled &&
	    wave_size >= graphics.min_subgroup_size && wave_size <= graphics.max_subgroup_size) {
		comp_subgroup_size.requiredSubgroupSize = wave_size;
		comp_shader_stage_info.pNext            = &comp_subgroup_size;
	}

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	AddLayoutBindings(descriptor_bindings, program, vk::ShaderStageFlagBits::eCompute);
	CreateDescriptorLayout(graphics, pipeline, descriptor_bindings);
	const vk::PushConstantRange push_constants {vk::ShaderStageFlagBits::eCompute, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};
	pipeline.push_constant_stages = vk::ShaderStageFlagBits::eCompute;

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &pipeline.descriptor_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_IF(pipeline.pipeline_layout != nullptr);

	LOGF("PipelineTrace: vkCreatePipelineLayout CS begin set_layouts=1 push_constants=%u\n",
	     1u);
	auto result = graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                  &pipeline.pipeline_layout);
	LOGF("PipelineTrace: vkCreatePipelineLayout CS done result=%s layout=%p\n",
	     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline_layout));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);

	vk::ComputePipelineCreateInfo info {};
	info.stage             = comp_shader_stage_info;
	info.layout            = pipeline.pipeline_layout;
	info.basePipelineIndex = -1;

	EXIT_IF(pipeline.pipeline != nullptr);

	LOGF("PipelineTrace: vkCreateComputePipelines begin layout=%p\n",
	     static_cast<void*>(pipeline.pipeline_layout));
	const auto guest_hash  = program.shader_hash;
	const bool capture_stats = PipelineStats::Wants(graphics, std::span(&guest_hash, 1));
	if (capture_stats) {
		info.flags |= PipelineStats::CaptureFlags(std::span(&guest_hash, 1));
	}
	const auto dump_record = ShaderDump::RecordComputePipeline(
	    graphics, info,
	    {descriptor_bindings, pipeline.uses_push_descriptors
	                              ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                              : vk::DescriptorSetLayoutCreateFlags {},
	     push_constants},
	    guest_hash);
	{
		// On a shader worker this is not a GPU-thread stall; it is reported as worker time.
		const Common::PerfStats::ScopedStall timing(
		    ShaderWorkerPool::OnWorkerThread() ? Common::PerfStats::StallKind::PipelineWorker
		                                       : Common::PerfStats::StallKind::PipelineCompile);
		// Use the same title-specific Vulkan compiler mutex as graphical
		// tessellation: compute pipeline compiles can otherwise overlap a
		// graphics worker despite the graphics-only worker serialization.
		std::string compute_title;
		const bool gt_compute =
		    Loader::SystemContentParamSfoGetString("TITLE_ID", &compute_title) &&
		    compute_title == "PPSA24156";
		std::unique_lock<std::mutex> serialize(
		    g_gt_native_pipeline_mutex, std::defer_lock);
		if (gt_compute) serialize.lock();
		result = graphics.device.createComputePipelines(driver_cache, 1, &info, nullptr,
		                                                &pipeline.pipeline);
	}
	LOGF("PipelineTrace: vkCreateComputePipelines done result=%s pipeline=%p\n",
	     vk::to_string(result).c_str(), static_cast<void*>(pipeline.pipeline));
	ShaderDump::RecordResult(dump_record, result);
	if (capture_stats && result == vk::Result::eSuccess) {
		PipelineStats::Write(graphics, pipeline.pipeline, fmt::format("cs_{:016x}", guest_hash),
		                     std::span(&guest_hash, 1));
	}
	if (result != vk::Result::eSuccess) {
		EXIT("vkCreateComputePipelines failed: %s, guest shader 0x%016" PRIx64 "%s%s\n",
		     vk::to_string(result).c_str(), guest_hash,
		     dump_record.empty() ? "" : ", dump record: ", dump_record.c_str());
	}

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);
}

} // namespace Libs::Graphics
