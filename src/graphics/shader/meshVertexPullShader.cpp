#include "graphics/shader/meshVertexPullShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <array>
#include <bit>
#include <utility>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::MeshEmulationField;
using ShaderRecompiler::IR::StageOutputKind;
using ShaderRecompiler::Spirv::Builder;

bool WritesLayerMirror(const MeshVertexPullStage& stage) {
	return stage.layer_viewport_location != UINT32_MAX &&
	       stage.layout->Has(StageOutputKind::Layer);
}

class MeshVertexPullEmitter {
public:
	explicit MeshVertexPullEmitter(const MeshVertexPullStage& stage_)
	    : stage(stage_), layout(*stage_.layout), builder(0x00010500u) {
		builder.AddMemoryModel(spv::AddressingModelPhysicalStorageBuffer64,
		                       spv::MemoryModelGLSL450);
		builder.RequireCapability(spv::CapabilityShader);
		builder.RequireCapability(spv::CapabilityInt64);
		builder.RequireCapability(spv::CapabilityPhysicalStorageBufferAddresses);

		void_type  = builder.Type(spv::OpTypeVoid);
		int_type   = builder.Type(spv::OpTypeInt, 32u, 1u);
		uint_type  = builder.Type(spv::OpTypeInt, 32u, 0u);
		u64_type   = builder.Type(spv::OpTypeInt, 64u, 0u);
		float_type = builder.Type(spv::OpTypeFloat, 32u);
		vec4_type  = builder.Type(spv::OpTypeVector, float_type, 4u);
		uvec2_type = builder.Type(spv::OpTypeVector, uint_type, 2u);
		bool_type  = builder.Type(spv::OpTypeBool);
	}

	std::vector<uint32_t> Emit() {
		main = builder.AllocateId();
		DefineInterface();
		builder.AddEntryPoint(spv::ExecutionModelVertex, main, "main", interfaces);
		builder.AddName(main, "main");

		builder.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone,
		                    builder.Type(spv::OpTypeFunction, void_type));
		builder.AddFunction(spv::OpLabel, builder.AllocateId());
		LoadRecordBase();
		Store(Access(Pointer(spv::StorageClassOutput, vec4_type), per_vertex, Int(0)),
		      LoadVec4(StageOutputKind::Position, 0));
		StoreDistances(StageOutputKind::ClipDistance, clip_member, layout.clip_distance_count);
		StoreDistances(StageOutputKind::CullDistance, cull_member, layout.cull_distance_count);
		if (layer_out != 0) {
			const auto layer = LoadWord(layout.Find(StageOutputKind::Layer, 0)->offset);
			Store(layer_out, layer);
			if (mirror_out != 0) {
				Store(mirror_out, Result(spv::OpCompositeConstruct, uvec2_type, layer, Uint(0)));
			}
		}
		for (const auto& [field, variable]: parameters) {
			Store(variable, LoadVec4(field->kind, field->index));
		}
		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

private:
	void DefineInterface() {
		vertex_index = Interface(spv::StorageClassInput, int_type, "gl_VertexIndex");
		BuiltIn(vertex_index, spv::BuiltInVertexIndex);

		// Push data dwords 0..1 hold the vertex record buffer address, dword 2 the record count
		// and dwords 3..4 the address of a counter of out-of-range vertex indices (0: none).
		const auto push_type = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		     {spv::OpMemberDecorate, {1, spv::DecorationOffset, 4}},
		     {spv::OpMemberDecorate, {2, spv::DecorationOffset, 8}},
		     {spv::OpMemberDecorate, {3, spv::DecorationOffset, 12}},
		     {spv::OpMemberDecorate, {4, spv::DecorationOffset, 16}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    uint_type, uint_type, uint_type, uint_type, uint_type);
		builder.AddName(push_type, "MeshVertexPullData");
		push_data = builder.DefineGlobalVariable(Pointer(spv::StorageClassPushConstant, push_type),
		                                         spv::StorageClassPushConstant);
		builder.AddName(push_data, "mesh_vertices");

		const auto words = builder.DecoratedType(
		    spv::OpTypeRuntimeArray,
		    {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}}, uint_type);
		const auto records = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		     {spv::OpMemberDecorate, {0, spv::DecorationNonWritable}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    words);
		records_pointer_type = Pointer(spv::StorageClassPhysicalStorageBuffer, records);
		word_pointer_type    = Pointer(spv::StorageClassPhysicalStorageBuffer, uint_type);

		std::vector<uint32_t>                          members {vec4_type};
		std::vector<std::pair<uint32_t, spv::BuiltIn>> builtins {{0u, spv::BuiltInPosition}};
		if (layout.clip_distance_count != 0) {
			builder.RequireCapability(spv::CapabilityClipDistance);
			clip_member = static_cast<uint32_t>(members.size());
			members.push_back(Array(float_type, layout.clip_distance_count));
			builtins.emplace_back(clip_member, spv::BuiltInClipDistance);
		}
		if (layout.cull_distance_count != 0) {
			builder.RequireCapability(spv::CapabilityCullDistance);
			cull_member = static_cast<uint32_t>(members.size());
			members.push_back(Array(float_type, layout.cull_distance_count));
			builtins.emplace_back(cull_member, spv::BuiltInCullDistance);
		}
		const auto per_vertex_type =
		    builder.Type(spv::OpTypeStruct, std::span<const uint32_t>(members));
		for (const auto& [member, builtin]: builtins) {
			builder.AddAnnotation(spv::OpMemberDecorate, per_vertex_type, member,
			                      spv::DecorationBuiltIn, builtin);
		}
		builder.AddAnnotation(spv::OpDecorate, per_vertex_type, spv::DecorationBlock);
		builder.AddName(per_vertex_type, "gl_PerVertex");
		per_vertex = Interface(spv::StorageClassOutput, per_vertex_type, "gl_out");

		if (layout.Has(StageOutputKind::Layer)) {
			builder.RequireCapability(spv::CapabilityShaderLayer);
			layer_out = Interface(spv::StorageClassOutput, uint_type, "gl_Layer");
			BuiltIn(layer_out, spv::BuiltInLayer);
			if (WritesLayerMirror(stage)) {
				mirror_out = Interface(spv::StorageClassOutput, uvec2_type, "out_layer_viewport");
				Location(mirror_out, stage.layer_viewport_location);
			}
		}
		for (const auto& field: layout.fields) {
			if (field.kind != StageOutputKind::Parameter) {
				continue;
			}
			EXIT_IF(WritesLayerMirror(stage) && field.location == stage.layer_viewport_location);
			const auto variable = Interface(spv::StorageClassOutput, vec4_type, "out_param");
			Location(variable, field.location);
			parameters.emplace_back(&field, variable);
		}
	}

	uint32_t PushWord(uint32_t dword) {
		return Load(uint_type, Access(Pointer(spv::StorageClassPushConstant, uint_type), push_data,
		                              Int(dword)));
	}

	uint32_t PushAddress(uint32_t dword) {
		return Result(spv::OpBitcast, u64_type,
		              Result(spv::OpCompositeConstruct, uvec2_type, PushWord(dword),
		                     PushWord(dword + 1u)));
	}

	// A vertex index past the records the compute stage wrote (stale or corrupt index data)
	// reads record 0 instead of memory outside the ring range, and is counted.
	void LoadRecordBase() {
		records          = Result(spv::OpConvertUToPtr, records_pointer_type, PushAddress(0));
		const auto index = Result(spv::OpBitcast, uint_type, Load(int_type, vertex_index));
		const auto bad   = Result(spv::OpUGreaterThanEqual, bool_type, index, PushWord(2));
		const auto counter_address = PushAddress(3);
		const auto count_it = Result(spv::OpLogicalAnd, bool_type, bad,
		                             Result(spv::OpINotEqual, bool_type, counter_address,
		                                    builder.Constant(spv::OpConstant, u64_type, 0u, 0u)));
		const auto then_label  = builder.AllocateId();
		const auto merge_label = builder.AllocateId();
		builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
		builder.AddFunction(spv::OpBranchConditional, count_it, then_label, merge_label);
		builder.AddFunction(spv::OpLabel, then_label);
		const auto counter = Result(spv::OpConvertUToPtr, word_pointer_type, counter_address);
		Result(spv::OpAtomicIIncrement, uint_type, counter, Uint(spv::ScopeDevice),
		       Uint(spv::MemorySemanticsMaskNone));
		builder.AddFunction(spv::OpBranch, merge_label);
		builder.AddFunction(spv::OpLabel, merge_label);
		const auto safe = Result(spv::OpSelect, uint_type, bad, Uint(0), index);
		record_base     = Result(spv::OpIMul, uint_type, safe, Uint(layout.stride_dwords));
	}

	uint32_t LoadWord(uint32_t offset) {
		const auto element = Result(spv::OpIAdd, uint_type, record_base, Uint(offset));
		const auto pointer = Access(word_pointer_type, records, Int(0), element);
		const auto value   = builder.AllocateId();
		builder.AddFunction(spv::OpLoad, uint_type, value, pointer, spv::MemoryAccessAlignedMask,
		                    static_cast<uint32_t>(sizeof(uint32_t)));
		return value;
	}

	uint32_t LoadFloat(uint32_t offset) {
		return Result(spv::OpBitcast, float_type, LoadWord(offset));
	}

	// A position the mesh stage did not export is (0, 0, 0, 0), as an unwritten mesh output.
	uint32_t LoadVec4(StageOutputKind kind, uint32_t index) {
		const auto* field = layout.Find(kind, index);
		if (field == nullptr) {
			const auto zero = Float(0.0f);
			return builder.Constant(spv::OpConstantComposite, vec4_type, zero, zero, zero, zero);
		}
		std::array<uint32_t, 4> components {};
		for (uint32_t component = 0; component < 4; component++) {
			components[component] = LoadFloat(field->offset + component);
		}
		return Result(spv::OpCompositeConstruct, vec4_type, components[0], components[1],
		              components[2], components[3]);
	}

	// Planes inside the declared count that the mesh stage does not export read as 0.
	void StoreDistances(StageOutputKind kind, uint32_t member, uint32_t count) {
		for (uint32_t plane = 0; plane < count; plane++) {
			const auto* field = layout.Find(kind, plane);
			const auto  value = field != nullptr ? LoadFloat(field->offset) : Float(0.0f);
			Store(Access(Pointer(spv::StorageClassOutput, float_type), per_vertex, Int(member),
			             Int(plane)),
			      value);
		}
	}

	uint32_t Interface(spv::StorageClass storage, uint32_t type, const char* name) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), storage);
		builder.AddName(variable, name);
		interfaces.push_back(variable);
		return variable;
	}

	void BuiltIn(uint32_t variable, spv::BuiltIn builtin) {
		builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
	}

	void Location(uint32_t variable, uint32_t location) {
		builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation, location);
	}

	uint32_t Array(uint32_t type, uint32_t size) {
		return builder.Type(spv::OpTypeArray, type, Uint(size));
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return builder.Type(spv::OpTypePointer, storage, type);
	}

	uint32_t Int(uint32_t value) { return builder.Constant(spv::OpConstant, int_type, value); }
	uint32_t Uint(uint32_t value) { return builder.Constant(spv::OpConstant, uint_type, value); }
	uint32_t Float(float value) {
		return builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(value));
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, type, id, operands...);
		return id;
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		return Result(spv::OpAccessChain, pointer_type, base, indices...);
	}

	uint32_t Load(uint32_t type, uint32_t pointer) { return Result(spv::OpLoad, type, pointer); }
	void Store(uint32_t pointer, uint32_t value) { builder.AddFunction(spv::OpStore, pointer, value); }

	const MeshVertexPullStage&                                  stage;
	const ShaderRecompiler::IR::MeshEmulationLayout&           layout;
	Builder                                                     builder;
	std::vector<uint32_t>                                       interfaces;
	std::vector<std::pair<const MeshEmulationField*, uint32_t>> parameters;
	uint32_t main                 = 0;
	uint32_t void_type            = 0;
	uint32_t int_type             = 0;
	uint32_t uint_type            = 0;
	uint32_t u64_type             = 0;
	uint32_t float_type           = 0;
	uint32_t vec4_type            = 0;
	uint32_t uvec2_type           = 0;
	uint32_t bool_type            = 0;
	uint32_t records_pointer_type = 0;
	uint32_t word_pointer_type    = 0;
	uint32_t vertex_index         = 0;
	uint32_t push_data            = 0;
	uint32_t per_vertex           = 0;
	uint32_t clip_member          = UINT32_MAX;
	uint32_t cull_member          = UINT32_MAX;
	uint32_t layer_out            = 0;
	uint32_t mirror_out           = 0;
	uint32_t records              = 0;
	uint32_t record_base          = 0;
};

} // namespace

std::vector<uint32_t> BuildMeshVertexPullShader(const MeshVertexPullStage& stage) {
	EXIT_IF(stage.layout == nullptr);
	return MeshVertexPullEmitter(stage).Emit();
}

ShaderRecompiler::IR::PreRasterOutputs MeshVertexPullOutputs(const MeshVertexPullStage& stage) {
	EXIT_IF(stage.layout == nullptr);
	ShaderRecompiler::IR::PreRasterOutputs outputs;
	outputs.clip_distance_count = stage.layout->clip_distance_count;
	outputs.cull_distance_count = stage.layout->cull_distance_count;
	outputs.layer               = stage.layout->Has(StageOutputKind::Layer);
	if (WritesLayerMirror(stage)) {
		outputs.layer_viewport_location = stage.layer_viewport_location;
	}
	return outputs;
}

uint32_t MeshVertexPullParameterMask(const MeshVertexPullStage& stage) {
	EXIT_IF(stage.layout == nullptr);
	uint32_t mask = 0;
	for (const auto& field: stage.layout->fields) {
		if (field.kind == StageOutputKind::Parameter && field.location < 32u) {
			mask |= 1u << field.location;
		}
	}
	return mask;
}

} // namespace Libs::Graphics
