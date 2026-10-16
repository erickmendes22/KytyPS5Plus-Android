#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Layer: return TypeU32(state);
		case IR::StageOutputKind::ClipDistance:
		case IR::StageOutputKind::CullDistance: return TypeF32(state);
		case IR::StageOutputKind::Position:
		case IR::StageOutputKind::Parameter: return TypeF32Vector(state, 4);
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
}

uint32_t MeshPushDword(EmitterState& state, uint32_t dword) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                          state.push_constant_variable, ConstantU32(state, 0),
	                          ConstantU32(state, dword));
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

// A uint32 array at a buffer device address from two push-data dwords.
uint32_t MeshEmulationBuffer(EmitterState& state, uint32_t low_dword) {
	const auto words = state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}},
	    TypeU32(state));
	const auto block = state.builder.DecoratedType(
	    spv::OpTypeStruct,
	    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	     {spv::OpDecorate, {spv::DecorationBlock}}},
	    words);
	const auto address =
	    PackU64(state, MeshPushDword(state, low_dword), MeshPushDword(state, low_dword + 1u));
	return Unary(state, spv::OpConvertUToPtr,
	             TypePointer(state, spv::StorageClassPhysicalStorageBuffer, block), address);
}

void MeshEmulationStore(EmitterState& state, uint32_t buffer, uint32_t index, uint32_t value) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePhysicalU32Pointer(state), pointer, buffer,
	                          ConstantU32(state, 0), index);
	state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask,
	                          static_cast<uint32_t>(sizeof(uint32_t)));
}

void MeshEmulationStoreTriangle(EmitterState& state, uint32_t buffer, uint32_t slot,
                                const std::array<uint32_t, 3>& vertices) {
	const auto base =
	    EmitBinaryU32(state, spv::OpIMul, slot, ConstantU32(state, IR::MeshEmulationIndexDwords));
	for (uint32_t corner = 0; corner < 3; corner++) {
		MeshEmulationStore(state, buffer, EmitAddU32(state, base, ConstantU32(state, corner)),
		                   vertices[corner]);
	}
}

// One vertex record: every export of the invocation's logical lane, as raw dwords.
void MeshEmulationStoreVertex(EmitterState& state, uint32_t buffer, uint32_t record,
                              uint32_t index, uint32_t half) {
	const auto& layout = state.mesh_emulation_layout;
	const auto  base   = EmitBinaryU32(state, spv::OpIMul, record,
	                                   ConstantU32(state, layout.stride_dwords));
	for (const auto& output: state.outputs) {
		const auto* field = layout.Find(output.kind, output.index);
		EXIT_IF(field == nullptr);
		const auto type   = MeshOutputType(state, output.kind);
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		const auto value  = shared ? MeshLoad(state, output.mesh_data_variable,
		                                      spv::StorageClassWorkgroup, type, index)
		                           : MeshLoad(state, output.mesh_data_variable,
		                                      spv::StorageClassPrivate, type,
		                                      ConstantU32(state, half));
		for (uint32_t component = 0; component < field->components; component++) {
			auto word = value;
			if (field->components > 1u) {
				word = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), word, value,
				                          component);
			}
			if (!shared) {
				word = Unary(state, spv::OpBitcast, TypeU32(state), word);
			}
			MeshEmulationStore(state, buffer,
			                   EmitAddU32(state, base, ConstantU32(state, field->offset + component)),
			                   word);
		}
	}
}

// The compute form of SetMeshOutputs and the output stores. Unused and culled triangle slots
// repeat the slot's first vertex; triangles naming a vertex past the allocation are culled.
void EmitMeshEmulationStores(EmitterState& state, uint32_t vertices, uint32_t primitives) {
	using IR::PushData;
	const auto& mesh       = state.input_info.vertex->mesh;
	const auto  threads    = mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
	EXIT_IF(threads == 0u || mesh.max_vertices == 0u || mesh.max_primitives == 0u);
	const auto vertex_buffer = MeshEmulationBuffer(state, PushData::MeshEmulationVertexAddressDword);
	const auto index_buffer  = MeshEmulationBuffer(state, PushData::MeshEmulationIndexAddressDword);
	const auto groups_x      = MeshPushDword(state, PushData::MeshEmulationGroupCountDword);
	const auto group_x  = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
	const auto group_y  = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
	const auto slot     = EmitAddU32(state, EmitBinaryU32(state, spv::OpIMul, group_y, groups_x),
	                                 group_x);
	const auto first_vertex =
	    EmitBinaryU32(state, spv::OpIMul, slot, ConstantU32(state, mesh.max_vertices));
	const auto first_primitive =
	    EmitBinaryU32(state, spv::OpIMul, slot, ConstantU32(state, mesh.max_primitives));
	// GS_ALLOC_REQ beyond the declared maximum would leave the workgroup's slot.
	const auto vertex_count = EmitGlsl<GLSLstd450UMin, IR::Type::U32>(
	    state, vertices, ConstantU32(state, mesh.max_vertices));
	const auto primitive_count = EmitGlsl<GLSLstd450UMin, IR::Type::U32>(
	    state, primitives, ConstantU32(state, mesh.max_primitives));
	const std::array<uint32_t, 3> degenerate {first_vertex, first_vertex, first_vertex};
	const auto rounds = (mesh.max_primitives + threads - 1u) / threads;
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half = half;
		const auto index = EmitLocalInvocationIndex(state);
		EmitIfCondition(state, Binary(state, spv::OpULessThan, TypeBool(state), index, vertex_count),
		                [&] {
			                MeshEmulationStoreVertex(state, vertex_buffer,
			                                         EmitAddU32(state, first_vertex, index), index,
			                                         half);
		                });
		const auto primitive_slot = Binary(state, spv::OpULessThan, TypeBool(state), index,
		                                   ConstantU32(state, mesh.max_primitives));
		EmitIfCondition(state, primitive_slot, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			auto live = Binary(state, spv::OpULessThan, TypeBool(state), index, primitive_count);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			live = Binary(state, spv::OpLogicalAnd, TypeBool(state), live,
			              Binary(state, spv::OpIEqual, TypeBool(state), null_bit,
			                     ConstantU32(state, 0)));
			std::array<uint32_t, 3> vertex {};
			for (uint32_t corner = 0; corner < 3; corner++) {
				vertex[corner] = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), vertex[corner],
				                          packed, ConstantU32(state, corner * 10u),
				                          ConstantU32(state, 10));
				live = Binary(state, spv::OpLogicalAnd, TypeBool(state), live,
				              Binary(state, spv::OpULessThan, TypeBool(state), vertex[corner],
				                     vertex_count));
			}
			for (uint32_t corner = 0; corner < 3; corner++) {
				vertex[corner] = Select(state, TypeU32(state), live,
				                        EmitAddU32(state, first_vertex, vertex[corner]),
				                        first_vertex);
			}
			MeshEmulationStoreTriangle(state, index_buffer, EmitAddU32(state, first_primitive, index),
			                           vertex);
		});
		// Triangle slots past the workgroup's invocations are always unused.
		for (uint32_t round = 1; round < rounds; round++) {
			const auto primitive = EmitAddU32(state, index, ConstantU32(state, round * threads));
			EmitIfCondition(state,
			                Binary(state, spv::OpULessThan, TypeBool(state), primitive,
			                       ConstantU32(state, mesh.max_primitives)),
			                [&] {
				                MeshEmulationStoreTriangle(
				                    state, index_buffer, EmitAddU32(state, first_primitive, primitive),
				                    degenerate);
			                });
		}
	}
}

} // namespace

void DefineMeshOutputs(EmitterState& state, uint32_t clip_distance_count,
                       uint32_t cull_distance_count) {
	const auto& mesh     = state.input_info.vertex->mesh;
	const bool  emulated = MeshEmulated(state);
	if (emulated) {
		state.mesh_emulation_layout =
		    IR::BuildMeshEmulationLayout(std::span<const IR::StageOutput>(state.program.info.outputs));
		state.program.info.mesh_emulation = true;
	}
	for (auto& output: state.outputs) {
		const auto type = MeshOutputType(state, output.kind);
		const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
		const bool cull = output.kind == IR::StageOutputKind::CullDistance;
		auto& variable = clip ? state.clip_distance_variable
		                 : cull ? state.cull_distance_variable : output.variable_id;
		// Emulation writes vertex records to a buffer instead of mesh output arrays.
		if (variable == 0 && !emulated) {
			const auto element_type = clip || cull
			                              ? state.builder.Type(spv::OpTypeArray, type,
			                                                   ConstantU32(state, clip ? clip_distance_count
			                                                                           : cull_distance_count))
			                              : type;
			variable = MeshArray(state, spv::StorageClassOutput, element_type,
			                     output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives
			                                                             : mesh.max_vertices);
			state.interface_variables.push_back(variable);
			state.builder.AddName(variable, output.debug_name.c_str());
			if (output.kind == IR::StageOutputKind::Parameter) {
				state.builder.AddAnnotation(spv::OpDecorate, variable,
				                            spv::DecorationLocation, output.location);
			} else {
				const auto builtin = clip ? spv::BuiltInClipDistance
				                     : cull ? spv::BuiltInCullDistance
				                     : output.kind == IR::StageOutputKind::Layer ? spv::BuiltInLayer
				                                                                 : spv::BuiltInPosition;
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
			}
			if (output.kind == IR::StageOutputKind::Layer) {
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
			}
		}
		output.variable_id = variable;
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	if (emulated) {
		return;
	}
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull =
	    MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
	                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = kind == IR::StageOutputKind::Layer;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	if (MeshEmulated(state)) {
		EmitMeshEmulationStores(state, vertices, primitives);
		state.lane_half = 0;
		state.builder.AddFunction(spv::OpReturn);
		state.builder.AddFunction(spv::OpFunctionEnd);
		return;
	}
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				uint32_t pointer;
				if (output.kind == IR::StageOutputKind::ClipDistance ||
				    output.kind == IR::StageOutputKind::CullDistance) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type), pointer,
					    output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				}
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          vertex[0], vertex[1], vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			state.builder.AddFunction(spv::OpStore,
			                          MeshElement(state, state.mesh_cull, spv::StorageClassOutput,
			                                      TypeBool(state), index),
			                          culled);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state),
				                            vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, layer);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
