#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

DppTargetLane EmitDppPermTargetLane(EmitterState& state, uint32_t subid, uint32_t control,
                                    uint32_t lane_bits) {
	const auto lane_mask  = (1u << lane_bits) - 1u;
	const auto group_base = state.builder.AllocateId();
	const auto lane      = state.builder.AllocateId();
	const auto shift     = state.builder.AllocateId();
	const auto selected0 = state.builder.AllocateId();
	const auto selected  = state.builder.AllocateId();
	const auto target    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), group_base, subid,
	                          ConstantU32(state, ~lane_mask));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane, subid,
	                          ConstantU32(state, lane_mask));
	state.builder.AddFunction(spv::OpIMul, TypeU32(state), shift, lane,
	                          ConstantU32(state, lane_bits));
	state.builder.AddFunction(spv::OpShiftRightLogical, TypeU32(state), selected0,
	                          ConstantU32(state, control), shift);
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), selected, selected0,
	                          ConstantU32(state, lane_mask));
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, group_base, selected);
	return {target, ConstantBool(state, true)};
}

DppTargetLane EmitDppRowShiftTargetLane(EmitterState& state, uint32_t subid, uint32_t amount,
                                        bool left) {
	const auto row          = state.builder.AllocateId();
	const auto lane         = state.builder.AllocateId();
	const auto lane_shifted = state.builder.AllocateId();
	const auto target       = state.builder.AllocateId();
	const auto valid        = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), row, subid,
	                          ConstantU32(state, 0xfffffff0u));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane, subid,
	                          ConstantU32(state, 15));
	if (left) {
		state.builder.AddFunction(spv::OpIAdd, TypeU32(state), lane_shifted, lane,
		                          ConstantU32(state, amount));
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), valid, lane,
		                          ConstantU32(state, 16u - amount));
	} else {
		state.builder.AddFunction(spv::OpISub, TypeU32(state), lane_shifted, lane,
		                          ConstantU32(state, amount));
		state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), valid, lane,
		                          ConstantU32(state, amount));
	}
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, row, lane_shifted);
	return {target, valid};
}

DppTargetLane EmitDppRowRotateRightTargetLane(EmitterState& state, uint32_t subid,
                                              uint32_t amount) {
	const auto row      = state.builder.AllocateId();
	const auto lane     = state.builder.AllocateId();
	const auto in_high  = state.builder.AllocateId();
	const auto minus    = state.builder.AllocateId();
	const auto plus     = state.builder.AllocateId();
	const auto selected = state.builder.AllocateId();
	const auto target   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), row, subid,
	                          ConstantU32(state, 0xfffffff0u));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane, subid,
	                          ConstantU32(state, 15));
	state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), in_high, lane,
	                          ConstantU32(state, amount));
	state.builder.AddFunction(spv::OpISub, TypeU32(state), minus, lane, ConstantU32(state, amount));
	state.builder.AddFunction(spv::OpIAdd, TypeU32(state), plus, lane,
	                          ConstantU32(state, 16u - amount));
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), selected, in_high, minus, plus);
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, row, selected);
	return {target, ConstantBool(state, true)};
}

DppTargetLane EmitDppMirrorTargetLane(EmitterState& state, uint32_t subid, bool half_row) {
	const auto base_mask = half_row ? 0xfffffff8u : 0xfffffff0u;
	const auto lane_mask = half_row ? 7u : 15u;
	const auto base      = state.builder.AllocateId();
	const auto lane      = state.builder.AllocateId();
	const auto mirrored  = state.builder.AllocateId();
	const auto target    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), base, subid,
	                          ConstantU32(state, base_mask));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane, subid,
	                          ConstantU32(state, lane_mask));
	state.builder.AddFunction(spv::OpISub, TypeU32(state), mirrored, ConstantU32(state, lane_mask),
	                          lane);
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, base, mirrored);
	return {target, ConstantBool(state, true)};
}

DppTargetLane EmitDppTargetLane(EmitterState& state, const IR::DppMoveFlags& flags) {
	const auto subid = EmitSubgroupLocalInvocationId(state);
	const auto control = flags.control;
	if (flags.dpp8) {
		return EmitDppPermTargetLane(state, subid, control, 3u);
	}
	if (control <= 0xffu) {
		return EmitDppPermTargetLane(state, subid, control, 2u);
	}
	if (control >= 0x101u && control <= 0x10fu) {
		return EmitDppRowShiftTargetLane(state, subid, control & 0xfu, true);
	}
	if (control >= 0x111u && control <= 0x11fu) {
		return EmitDppRowShiftTargetLane(state, subid, control & 0xfu, false);
	}
	if (control >= 0x121u && control <= 0x12fu) {
		return EmitDppRowRotateRightTargetLane(state, subid, control & 0xfu);
	}
	if (control == 0x140u) {
		return EmitDppMirrorTargetLane(state, subid, false);
	}
	if (control == 0x141u) {
		return EmitDppMirrorTargetLane(state, subid, true);
	}
	// row_share:N reads lane N of the caller's row of 16.
	if (control >= 0x150u && control <= 0x15fu) {
		const auto row    = state.builder.AllocateId();
		const auto target = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), row, subid,
		                          ConstantU32(state, 0xfffffff0u));
		state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, row,
		                          ConstantU32(state, control & 0xfu));
		return {target, ConstantBool(state, true)};
	}
	if (control >= 0x160u && control <= 0x16fu) {
		const auto target = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpBitwiseXor, TypeU32(state), target, subid,
		                          ConstantU32(state, control & 0xfu));
		return {target, ConstantBool(state, true)};
	}
	return {subid, ConstantBool(state, true)};
}

uint32_t EmitSubgroupLocalInvocationId(EmitterState& state) {
	if (state.subgroup_isolated) {
		return ConstantU32(state, 0);
	}
	if (state.subgroup_local_invocation_id_variable == 0) {
		EXIT("SubgroupLocalInvocationId was not declared before SPIR-V function emission\n");
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value,
	                          state.subgroup_local_invocation_id_variable);
	return state.lane_half == 0 ? value : EmitAddU32(state, value, ConstantU32(state, 32));
}

uint32_t FixedSubgroupSize(const EmitterState& state) {
	const auto& features = state.device_features;
	// Only a required subgroup size pins it: graphics stages report the device's subgroupSize
	// (128 on Turnip) but their executables run 64-wide there.
	if (!features.constant_lane_broadcast || state.program.stage != ShaderType::Compute) return 0;
	// CreatePipelineInternal requires the program's wave size when it is in range.
	const auto wave = state.program.wave_size;
	return features.compute_required_subgroup_max != 0 &&
	               wave >= features.compute_required_subgroup_min &&
	               wave <= features.compute_required_subgroup_max
	           ? wave
	           : 0u;
}

uint32_t EmitSubgroupSize(EmitterState& state) {
	if (const auto fixed = FixedSubgroupSize(state); fixed != 0) {
		return ConstantU32(state, fixed);
	}
	if (state.subgroup_size_variable == 0) {
		const auto variable = DefineInterfaceVariable(state, TypeU32(state), spv::StorageClassInput,
		                                              "gl_SubgroupSize");
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn,
		                            spv::BuiltInSubgroupSize);
		if (state.program.stage == ShaderType::Pixel) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationFlat);
		}
		state.subgroup_size_variable = variable;
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, state.subgroup_size_variable);
	return value;
}

uint32_t InputVariableForKind(const EmitterState& state, IR::StageInputKind kind) {
	for (const auto& input: state.inputs) {
		if (input.kind == kind) {
			return input.variable_id;
		}
	}
	return 0;
}

const InputBinding* InputBindingForParameter(const EmitterState& state, uint32_t location) {
	for (const auto& input: state.inputs) {
		if (input.kind == IR::StageInputKind::Parameter && input.location == location) {
			return &input;
		}
	}
	return nullptr;
}

uint32_t EmitInputComponentU32(EmitterState& state, IR::StageInputKind kind, uint32_t component) {
	const auto variable = InputVariableForKind(state, kind);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto pointer = state.builder.AllocateId();
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypePointer(state, spv::StorageClassInput, TypeU32(state)), pointer,
	                          variable, ConstantU32(state, component));
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	const bool packed = state.program.info.group_pack > 1u && component == 0u;
	if (kind == IR::StageInputKind::WorkgroupId && component < 3u) {
		// Packed: host group x holds guest groups 2x (lanes 0-31) and 2x+1 (lanes 32-63).
		const auto group = packed ? EmitAddU32(state,
		                                       EmitBinaryU32(state, spv::OpShiftLeftLogical, value,
		                                                     ConstantU32(state, 1u)),
		                                       EmitPackedGroupHalf(state))
		                          : value;
		return EmitAddU32(state, group, EmitGroupBaseU32(state, component));
	}
	if (kind == IR::StageInputKind::NumWorkgroups && packed) {
		return EmitBinaryU32(state, spv::OpShiftLeftLogical, value, ConstantU32(state, 1u));
	}
	return value;
}

uint32_t EmitPackedGroupHalf(EmitterState& state) {
	const auto variable = InputVariableForKind(state, IR::StageInputKind::LocalInvocationIndex);
	if (variable == 0) {
		EXIT("packed compute groups need gl_LocalInvocationIndex\n");
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, variable);
	return EmitBinaryU32(state, spv::OpShiftRightLogical, value, ConstantU32(state, 5u));
}

uint32_t EmitGroupBaseU32(EmitterState& state, uint32_t component) {
	const auto start = state.program.bindings.group_base_dword;
	if (start == IR::PushData::NoStart) {
		return ConstantU32(state, 0);
	}
	return EmitShaderDataDwordLoad(state, start + component);
}

uint32_t EmitLocalInvocationIndex(EmitterState& state) {
	const auto variable = InputVariableForKind(state, IR::StageInputKind::LocalInvocationIndex);
	if (variable == 0) {
		return ConstantU32(state, 0);
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, variable);
	if (state.program.info.group_pack > 1u) {
		// Each 32-lane half of the host workgroup is one guest workgroup.
		return EmitBinaryU32(state, spv::OpBitwiseAnd, value, ConstantU32(state, 31u));
	}
	if (state.lane_count == 2) {
		const auto wave_base =
		    EmitBinaryU32(state, spv::OpBitwiseAnd, value, ConstantU32(state, ~31u));
		return EmitAddU32(state, EmitAddU32(state, value, wave_base),
		                  ConstantU32(state, state.lane_half * 32));
	}
	return value;
}

uint32_t EmitVertexParameterComponentU32(EmitterState& state, const InputBinding& input,
                                         uint32_t component) {
	const auto count = VertexParameterComponentCount(input);
	const auto kind  = VertexParameterScalarKind(state, input.location);
	const auto scalar_type = VertexParameterScalarType(state, kind);
	uint32_t   raw         = state.builder.AllocateId();
	if (count == 1u) {
		state.builder.AddFunction(spv::OpLoad, scalar_type, raw, input.variable_id);
	} else {
		const auto pointer_type = TypePointer(state, spv::StorageClassInput, scalar_type);
		const auto pointer      = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, pointer_type, pointer, input.variable_id,
		                          ConstantU32(state, component));
		state.builder.AddFunction(spv::OpLoad, scalar_type, raw, pointer);
	}

	if (kind == VertexInputScalarKind::Uint) {
		return raw;
	}

	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, raw);
	return bits;
}

uint32_t EmitSubgroupBallot(EmitterState& state, uint32_t predicate) {
	const auto type = TypeU32Vector(state, 4);
	const auto result = state.builder.AllocateId();
	if (state.subgroup_isolated) {
		// Lane 0 of a one-lane wave: bit 0 carries the predicate.
		const auto zero = ConstantU32(state, 0);
		const auto bit  = Select(state, TypeU32(state), predicate, ConstantU32(state, 1), zero);
		state.builder.AddFunction(spv::OpCompositeConstruct, type, result, bit, zero, zero, zero);
		return result;
	}
	state.builder.AddFunction(spv::OpGroupNonUniformBallot, type, result,
	                          ConstantU32(state, spv::ScopeSubgroup), predicate);
	return result;
}

uint32_t EmitSubgroupShuffle(EmitterState& state, uint32_t type, uint32_t value, uint32_t lane) {
	if (state.subgroup_isolated) {
		// The only lane is the invocation itself.
		return value;
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, type, result,
	                          ConstantU32(state, spv::ScopeSubgroup), value, lane);
	return result;
}

uint32_t EmitSubgroupLaneActiveBool(EmitterState& state, uint32_t lane) {
	return EmitBallotLaneActiveBool(state, EmitSubgroupBallot(state, ConstantBool(state, true)),
	                                lane);
}
bool WaveSliced(const EmitterState& state) {
	// Packed compute groups (KYTY_CS_PACK) put one guest wave in each 32-lane half.
	return (state.device_features.wave_slices || state.program.info.group_pack > 1u) &&
	       !state.subgroup_isolated && state.lane_count == 1u &&
	       state.program.stage != ShaderType::TessellationControl;
}

uint32_t EmitWaveLocalLane(EmitterState& state) {
	return EmitBinaryU32(state, spv::OpBitwiseAnd, EmitSubgroupLocalInvocationId(state),
	                     ConstantU32(state, state.program.wave_size - 1u));
}

uint32_t EmitWaveLaneBase(EmitterState& state) {
	return EmitBinaryU32(state, spv::OpBitwiseAnd, EmitSubgroupLocalInvocationId(state),
	                     ConstantU32(state, ~(state.program.wave_size - 1u)));
}

uint32_t EmitWaveBallot(EmitterState& state, uint32_t subgroup_ballot) {
	const auto word = [&](uint32_t index) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpVectorExtractDynamic, TypeU32(state), result,
		                          subgroup_ballot, index);
		return result;
	};
	const auto first_word = EmitBinaryU32(state, spv::OpShiftRightLogical, EmitWaveLaneBase(state),
	                                      ConstantU32(state, 5));
	const auto zero = ConstantU32(state, 0);
	const auto low  = word(first_word);
	const auto high = state.program.wave_size == 64u
	                      ? word(EmitAddU32(state, first_word, ConstantU32(state, 1)))
	                      : zero;
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4), result, low, high,
	                          zero, zero);
	return result;
}

uint32_t EmitWaveLaneToSubgroupLane(EmitterState& state, uint32_t wave_lane) {
	return EmitBinaryU32(state, spv::OpBitwiseOr, EmitWaveLaneBase(state),
	                     EmitBinaryU32(state, spv::OpBitwiseAnd, wave_lane,
	                                   ConstantU32(state, state.program.wave_size - 1u)));
}

uint32_t EmitBallotLaneActiveBool(EmitterState& state, uint32_t active_ballot, uint32_t lane) {
	if (WaveSliced(state)) {
		// Host lane numbers index the whole subgroup ballot.
		const auto word = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpVectorExtractDynamic, TypeU32(state), word, active_ballot,
		                          EmitBinaryU32(state, spv::OpShiftRightLogical, lane,
		                                        ConstantU32(state, 5)));
		const auto bit = EmitBinaryU32(
		    state, spv::OpBitwiseAnd,
		    EmitBinaryU32(state, spv::OpShiftRightLogical, word,
		                  EmitBinaryU32(state, spv::OpBitwiseAnd, lane, ConstantU32(state, 31))),
		    ConstantU32(state, 1));
		const auto active = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), active, bit,
		                          ConstantU32(state, 0));
		return active;
	}
	const auto low = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, active_ballot, 0);
	uint32_t mask = low;
	if (state.program.wave_size == 64u) {
		const auto high     = state.builder.AllocateId();
		const auto in_high  = state.builder.AllocateId();
		const auto selected = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, active_ballot, 1);
		state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), in_high, lane,
		                          ConstantU32(state, 32));
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), selected, in_high, high, low);
		mask = selected;
	}

	const auto lane_low = state.builder.AllocateId();
	const auto bit      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane_low, lane,
	                          ConstantU32(state, 31));
	state.builder.AddFunction(spv::OpShiftLeftLogical, TypeU32(state), bit, ConstantU32(state, 1),
	                          lane_low);

	const auto hit      = state.builder.AllocateId();
	const auto active   = state.builder.AllocateId();
	const auto in_range = state.builder.AllocateId();
	const auto ret      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), hit, mask, bit);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), active, hit,
	                          ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpULessThan, TypeBool(state), in_range, lane,
	                          ConstantU32(state, state.program.wave_size));
	state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), ret, active, in_range);
	return ret;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
