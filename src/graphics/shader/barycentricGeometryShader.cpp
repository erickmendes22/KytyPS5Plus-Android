#include "graphics/shader/barycentricGeometryShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <array>
#include <bit>
#include <map>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::EmulatedVaryingKind;
using ShaderRecompiler::Spirv::Builder;

constexpr uint32_t MaxVertices = 3;

uint32_t VertexCount(BarycentricPrimitive primitive) {
	switch (primitive) {
		case BarycentricPrimitive::Points: return 1;
		case BarycentricPrimitive::Lines: return 2;
		case BarycentricPrimitive::Triangles: return 3;
	}
	return 3;
}

class BarycentricGeometryEmitter {
public:
	explicit BarycentricGeometryEmitter(const BarycentricGeometryStage& stage_)
	    : stage(stage_), vertex_count(VertexCount(stage_.primitive)) {
		EXIT_IF(stage.fragment == nullptr);
		builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
		builder.RequireCapability(spv::CapabilityShader);
		builder.RequireCapability(spv::CapabilityGeometry);

		void_type  = builder.Type(spv::OpTypeVoid);
		int_type   = builder.Type(spv::OpTypeInt, 32u, 1u);
		uint_type  = builder.Type(spv::OpTypeInt, 32u, 0u);
		float_type = builder.Type(spv::OpTypeFloat, 32u);
		vec3_type  = builder.Type(spv::OpTypeVector, float_type, 3u);
		vec4_type  = builder.Type(spv::OpTypeVector, float_type, 4u);
		uvec2_type = builder.Type(spv::OpTypeVector, uint_type, 2u);
		uvec4_type = builder.Type(spv::OpTypeVector, uint_type, 4u);
		vertices_type =
		    builder.Type(spv::OpTypeArray, uvec4_type, Uint(MaxVertices)); // PerVertex payload
		per_vertex_type = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    vec4_type);
	}

	std::vector<uint32_t> Emit() {
		main = builder.AllocateId();
		DefineExecutionModes();
		DefineInputs();
		DefineOutputs();
		builder.AddEntryPoint(spv::ExecutionModelGeometry, main, "main", interfaces);
		builder.AddName(main, "main");

		builder.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone,
		                    builder.Type(spv::OpTypeFunction, void_type));
		builder.AddFunction(spv::OpLabel, builder.AllocateId());
		const auto payloads = LoadPerVertexPayloads();
		const auto mirrors  = LoadLayerViewportMirrors();
		for (uint32_t vertex = 0; vertex < vertex_count; vertex++) {
			EmitVertex(vertex, payloads, mirrors);
		}
		builder.AddFunction(spv::OpEndPrimitive);
		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

private:
	struct VaryingOutput {
		const ShaderRecompiler::IR::EmulatedVarying* varying  = nullptr;
		uint32_t                                     variable = 0;
	};

	void DefineExecutionModes() {
		switch (stage.primitive) {
			case BarycentricPrimitive::Points:
				builder.AddExecutionMode(main, spv::ExecutionModeInputPoints);
				builder.AddExecutionMode(main, spv::ExecutionModeOutputPoints);
				break;
			case BarycentricPrimitive::Lines:
				builder.AddExecutionMode(main, spv::ExecutionModeInputLines);
				builder.AddExecutionMode(main, spv::ExecutionModeOutputLineStrip);
				break;
			case BarycentricPrimitive::Triangles:
				builder.AddExecutionMode(main, spv::ExecutionModeTriangles);
				builder.AddExecutionMode(main, spv::ExecutionModeOutputTriangleStrip);
				break;
		}
		builder.AddExecutionMode(main, spv::ExecutionModeInvocations, 1u);
		builder.AddExecutionMode(main, spv::ExecutionModeOutputVertices, vertex_count);
	}

	bool ForwardsPointSize() const {
		return stage.primitive == BarycentricPrimitive::Points && stage.previous.point_size &&
		       stage.point_size;
	}

	bool PreviousWrites(uint32_t location) const {
		return location < 32u && (stage.previous_locations & (1u << location)) != 0;
	}

	void DefineInputs() {
		gl_in = Interface(spv::StorageClassInput, Array(per_vertex_type, vertex_count), "gl_in");
		const auto& previous = stage.previous;
		if (previous.clip_distance_count != 0) {
			builder.RequireCapability(spv::CapabilityClipDistance);
			clip_in = Interface(spv::StorageClassInput,
			                    Array(ClipArray(previous.clip_distance_count), vertex_count),
			                    "gl_in_ClipDistance");
			BuiltIn(clip_in, spv::BuiltInClipDistance);
		}
		if (previous.cull_distance_count != 0) {
			builder.RequireCapability(spv::CapabilityCullDistance);
			cull_in = Interface(spv::StorageClassInput,
			                    Array(ClipArray(previous.cull_distance_count), vertex_count),
			                    "gl_in_CullDistance");
			BuiltIn(cull_in, spv::BuiltInCullDistance);
		}
		if (ForwardsPointSize()) {
			builder.RequireCapability(spv::CapabilityGeometryPointSize);
			point_size_in = Interface(spv::StorageClassInput, Array(float_type, vertex_count),
			                          "gl_in_PointSize");
			BuiltIn(point_size_in, spv::BuiltInPointSize);
		}
		if (previous.layer_viewport_location != UINT32_MAX) {
			mirror_in = Interface(spv::StorageClassInput, Array(uvec2_type, vertex_count),
			                      "in_layer_viewport");
			Location(mirror_in, previous.layer_viewport_location);
		}
		if (stage.fragment->primitive_id) {
			primitive_id_in = Interface(spv::StorageClassInput, int_type, "gl_PrimitiveIDIn");
			BuiltIn(primitive_id_in, spv::BuiltInPrimitiveId);
		}
		for (const auto& varying: stage.fragment->varyings) {
			const auto source = varying.source_location;
			if (source == UINT32_MAX || !PreviousWrites(source) || sources.contains(source)) {
				continue;
			}
			const auto variable = Interface(spv::StorageClassInput, Array(vec4_type, vertex_count),
			                                "in_param");
			Location(variable, source);
			sources.emplace(source, variable);
		}
	}

	void DefineOutputs() {
		gl_out               = Interface(spv::StorageClassOutput, per_vertex_type, "gl_out");
		const auto& previous = stage.previous;
		if (clip_in != 0) {
			clip_out = Interface(spv::StorageClassOutput, ClipArray(previous.clip_distance_count),
			                     "gl_ClipDistance");
			BuiltIn(clip_out, spv::BuiltInClipDistance);
		}
		if (cull_in != 0) {
			cull_out = Interface(spv::StorageClassOutput, ClipArray(previous.cull_distance_count),
			                     "gl_CullDistance");
			BuiltIn(cull_out, spv::BuiltInCullDistance);
		}
		if (point_size_in != 0) {
			point_size_out = Interface(spv::StorageClassOutput, float_type, "gl_PointSize");
			BuiltIn(point_size_out, spv::BuiltInPointSize);
		}
		if (mirror_in != 0 && previous.layer) {
			layer_out = Interface(spv::StorageClassOutput, uint_type, "gl_Layer");
			BuiltIn(layer_out, spv::BuiltInLayer);
		}
		if (mirror_in != 0 && previous.viewport_index) {
			builder.RequireCapability(spv::CapabilityMultiViewport);
			viewport_out = Interface(spv::StorageClassOutput, uint_type, "gl_ViewportIndex");
			BuiltIn(viewport_out, spv::BuiltInViewportIndex);
		}
		if (primitive_id_in != 0) {
			primitive_id_out = Interface(spv::StorageClassOutput, int_type, "gl_PrimitiveID");
			BuiltIn(primitive_id_out, spv::BuiltInPrimitiveId);
		}
		for (const auto& varying: stage.fragment->varyings) {
			uint32_t type = vec4_type;
			switch (varying.kind) {
				case EmulatedVaryingKind::Interpolated: break;
				case EmulatedVaryingKind::PerVertex: type = vertices_type; break;
				case EmulatedVaryingKind::BaryCoord:
				case EmulatedVaryingKind::BaryCoordNoPerspective: type = vec3_type; break;
			}
			const auto variable = Interface(spv::StorageClassOutput, type, "out_varying");
			Location(variable, varying.location);
			outputs.push_back({&varying, variable});
		}
	}

	// Every output vertex carries the same copies of all input vertices. Vertices a points or
	// lines primitive lacks repeat its last vertex.
	std::map<uint32_t, uint32_t> LoadPerVertexPayloads() {
		std::map<uint32_t, uint32_t> payloads;
		for (const auto& output: outputs) {
			if (output.varying->kind != EmulatedVaryingKind::PerVertex) {
				continue;
			}
			std::array<uint32_t, MaxVertices> values {};
			for (uint32_t vertex = 0; vertex < MaxVertices; vertex++) {
				values[vertex] = vertex < vertex_count
				                     ? LoadSourceBits(output.varying->source_location, vertex)
				                     : values[vertex_count - 1u];
			}
			payloads.emplace(output.variable, Result(spv::OpCompositeConstruct, vertices_type,
			                                         values[0], values[1], values[2]));
		}
		return payloads;
	}

	std::array<uint32_t, MaxVertices> LoadLayerViewportMirrors() {
		std::array<uint32_t, MaxVertices> mirrors {};
		if (layer_out == 0 && viewport_out == 0) {
			return mirrors;
		}
		for (uint32_t vertex = 0; vertex < vertex_count; vertex++) {
			mirrors[vertex] =
			    Load(uvec2_type, Access(Pointer(spv::StorageClassInput, uvec2_type), mirror_in,
			                            Int(vertex)));
		}
		return mirrors;
	}

	void EmitVertex(uint32_t vertex, const std::map<uint32_t, uint32_t>& payloads,
	                const std::array<uint32_t, MaxVertices>& mirrors) {
		const auto position = Load(
		    vec4_type, Access(Pointer(spv::StorageClassInput, vec4_type), gl_in, Int(vertex), Int(0)));
		Store(Access(Pointer(spv::StorageClassOutput, vec4_type), gl_out, Int(0)), position);
		CopyDistances(clip_in, clip_out, stage.previous.clip_distance_count, vertex);
		CopyDistances(cull_in, cull_out, stage.previous.cull_distance_count, vertex);
		if (point_size_out != 0) {
			Store(point_size_out,
			      Load(float_type, Access(Pointer(spv::StorageClassInput, float_type),
			                              point_size_in, Int(vertex))));
		}
		if (layer_out != 0) {
			Store(layer_out, Result(spv::OpCompositeExtract, uint_type, mirrors[vertex], 0u));
		}
		if (viewport_out != 0) {
			Store(viewport_out, Result(spv::OpCompositeExtract, uint_type, mirrors[vertex], 1u));
		}
		if (primitive_id_out != 0) {
			Store(primitive_id_out, Load(int_type, primitive_id_in));
		}
		for (const auto& output: outputs) {
			const auto& varying = *output.varying;
			switch (varying.kind) {
				case EmulatedVaryingKind::Interpolated:
					Store(output.variable, LoadSource(varying.source_location, vertex));
					break;
				case EmulatedVaryingKind::PerVertex:
					Store(output.variable, payloads.at(output.variable));
					break;
				case EmulatedVaryingKind::BaryCoord:
				case EmulatedVaryingKind::BaryCoordNoPerspective: {
					std::array<uint32_t, MaxVertices> weights {};
					for (uint32_t i = 0; i < MaxVertices; i++) {
						weights[i] = Float(i == vertex ? 1.0f : 0.0f);
					}
					Store(output.variable, builder.Constant(spv::OpConstantComposite, vec3_type,
					                                        weights[0], weights[1], weights[2]));
					break;
				}
			}
		}
		builder.AddFunction(spv::OpEmitVertex);
	}

	void CopyDistances(uint32_t input, uint32_t output, uint32_t count, uint32_t vertex) {
		if (input == 0) {
			return;
		}
		const auto type = ClipArray(count);
		Store(output, Load(type, Access(Pointer(spv::StorageClassInput, type), input, Int(vertex))));
	}

	uint32_t LoadSource(uint32_t location, uint32_t vertex) {
		const auto source = sources.find(location);
		if (source == sources.end()) {
			const auto zero = Float(0.0f);
			return builder.Constant(spv::OpConstantComposite, vec4_type, zero, zero, zero, zero);
		}
		return Load(vec4_type,
		            Access(Pointer(spv::StorageClassInput, vec4_type), source->second, Int(vertex)));
	}

	uint32_t LoadSourceBits(uint32_t location, uint32_t vertex) {
		if (!sources.contains(location)) {
			const auto zero = Uint(0);
			return builder.Constant(spv::OpConstantComposite, uvec4_type, zero, zero, zero, zero);
		}
		return Result(spv::OpBitcast, uvec4_type, LoadSource(location, vertex));
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

	uint32_t ClipArray(uint32_t count) { return Array(float_type, count); }

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

	const BarycentricGeometryStage& stage;
	const uint32_t                  vertex_count;
	Builder                         builder;
	std::vector<uint32_t>           interfaces;
	std::map<uint32_t, uint32_t>    sources; // Previous-stage location -> vec4[N] input.
	std::vector<VaryingOutput>      outputs;
	uint32_t                        main             = 0;
	uint32_t                        void_type        = 0;
	uint32_t                        int_type         = 0;
	uint32_t                        uint_type        = 0;
	uint32_t                        float_type       = 0;
	uint32_t                        vec3_type        = 0;
	uint32_t                        vec4_type        = 0;
	uint32_t                        uvec2_type       = 0;
	uint32_t                        uvec4_type       = 0;
	uint32_t                        vertices_type    = 0;
	uint32_t                        per_vertex_type  = 0;
	uint32_t                        gl_in            = 0;
	uint32_t                        gl_out           = 0;
	uint32_t                        clip_in          = 0;
	uint32_t                        clip_out         = 0;
	uint32_t                        cull_in          = 0;
	uint32_t                        cull_out         = 0;
	uint32_t                        point_size_in    = 0;
	uint32_t                        point_size_out   = 0;
	uint32_t                        mirror_in        = 0;
	uint32_t                        layer_out        = 0;
	uint32_t                        viewport_out     = 0;
	uint32_t                        primitive_id_in  = 0;
	uint32_t                        primitive_id_out = 0;
};

} // namespace

std::vector<uint32_t> BuildBarycentricGeometryShader(const BarycentricGeometryStage& stage) {
	return BarycentricGeometryEmitter(stage).Emit();
}

uint32_t BarycentricGeometryOutputComponents(const BarycentricGeometryStage& stage) {
	EXIT_IF(stage.fragment == nullptr);
	uint32_t components = 4u; // Position
	for (const auto& varying: stage.fragment->varyings) {
		components += varying.kind == EmulatedVaryingKind::PerVertex ? 4u * MaxVertices : 4u;
	}
	return components;
}

} // namespace Libs::Graphics
