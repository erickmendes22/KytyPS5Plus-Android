// KYTY_LOOP_GUARD (graphics/shader/loopGuard.h): per-invocation iteration counting through
// guarded loop headers, the forced exit condition and the trip log.
#include "graphics/shader/halfRateTiles.h"
#include "graphics/shader/loopGuard.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

namespace {

uint32_t FunctionU32Pointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassFunction, TypeU32(state));
}

uint32_t LoadU32(EmitterState& state, uint32_t variable) {
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, variable);
	return value;
}

// A uint32 runtime array at a constant buffer device address.
uint32_t GuardLogWords(EmitterState& state) {
	const auto words = state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}},
	    TypeU32(state));
	const auto block = state.builder.DecoratedType(
	    spv::OpTypeStruct,
	    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}}, {spv::OpDecorate, {spv::DecorationBlock}}},
	    words);
	return Unary(state, spv::OpConvertUToPtr, TypePointer(state, spv::StorageClassPhysicalStorageBuffer, block),
	             ConstantU64(state, state.loop_guard_log_address));
}

uint32_t GuardLogWord(EmitterState& state, uint32_t log, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePhysicalU32Pointer(state), pointer, log,
	                          ConstantU32(state, 0), index);
	return pointer;
}

// A uint32 runtime array at a constant buffer device address.
uint32_t LogWordsAt(EmitterState& state, uint64_t address) {
	const auto words = state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}},
	    TypeU32(state));
	const auto block = state.builder.DecoratedType(
	    spv::OpTypeStruct,
	    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}}, {spv::OpDecorate, {spv::DecorationBlock}}},
	    words);
	return Unary(state, spv::OpConvertUToPtr, TypePointer(state, spv::StorageClassPhysicalStorageBuffer, block),
	             ConstantU64(state, address));
}

} // namespace

// KYTY_RT_HALF_CHECK: one word of the check log, in lanes with exec set.
void EmitDebugLogStoreU32(EmitterState& state, uint32_t index, uint32_t value, uint32_t exec) {
	if (state.debug_log_address == 0) {
		return;
	}
	EmitIfCondition(state, exec, [&] {
		const auto log     = LogWordsAt(state, state.debug_log_address);
		const auto pointer = GuardLogWord(state, log, index);
		state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask,
		                          static_cast<uint32_t>(sizeof(uint32_t)));
	});
}

// rt_half=2 re-trace buffer (graphics/shader/halfRateTiles.h).
void EmitRetraceStoreU32(EmitterState& state, uint32_t index, uint32_t value, uint32_t exec) {
	if (state.retrace_address == 0) {
		return;
	}
	EmitIfCondition(state, exec, [&] {
		const auto pointer = GuardLogWord(state, LogWordsAt(state, state.retrace_address), index);
		state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask,
		                          static_cast<uint32_t>(sizeof(uint32_t)));
	});
}

uint32_t EmitRetraceLoadU32(EmitterState& state, uint32_t index) {
	if (state.retrace_address == 0) {
		return ConstantU32(state, 0);
	}
	const auto pointer = GuardLogWord(state, LogWordsAt(state, state.retrace_address), index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer, spv::MemoryAccessAlignedMask,
	                          static_cast<uint32_t>(sizeof(uint32_t)));
	return value;
}

void EmitRetraceAppendU32(EmitterState& state, uint32_t value, uint32_t exec) {
	if (state.retrace_address == 0) {
		return;
	}
	EmitIfCondition(state, exec, [&] {
		const auto words = LogWordsAt(state, state.retrace_address);
		const auto slot  = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAtomicIAdd, TypeU32(state), slot, GuardLogWord(state, words, ConstantU32(state, 0)),
		                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, spv::MemorySemanticsMaskNone),
		                          ConstantU32(state, 1));
		const auto room = Binary(state, spv::OpULessThan, TypeBool(state), slot, ConstantU32(state, HalfRateTiles::kRetraceEntries));
		EmitIfCondition(state, room, [&] {
			const auto pointer = GuardLogWord(state, words, EmitAddU32(state, slot, ConstantU32(state, 4)));
			state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask,
			                          static_cast<uint32_t>(sizeof(uint32_t)));
		});
	});
}

void EmitLoopGuardVariables(EmitterState& state) {
	if (!state.loop_guard) {
		return;
	}
	for (const auto variable: {state.loop_guard_counter, state.loop_guard_logged}) {
		state.builder.AddFunction(spv::OpVariable, FunctionU32Pointer(state), variable,
		                          spv::StorageClassFunction);
	}
}

void EmitLoopGuardInit(EmitterState& state) {
	if (!state.loop_guard) {
		return;
	}
	state.builder.AddFunction(spv::OpStore, state.loop_guard_counter, ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpStore, state.loop_guard_logged, ConstantU32(state, 0));
}

void EmitLoopGuardCount(EmitterState& state) {
	const auto next = EmitAddU32(state, LoadU32(state, state.loop_guard_counter), ConstantU32(state, 1));
	state.builder.AddFunction(spv::OpStore, state.loop_guard_counter, next);
}

uint32_t EmitLoopGuardTripped(EmitterState& state, uint32_t factor) {
	const uint64_t limit = static_cast<uint64_t>(LoopGuard::Limit()) * factor;
	return Binary(state, spv::OpUGreaterThan, TypeBool(state), LoadU32(state, state.loop_guard_counter),
	              ConstantU32(state, static_cast<uint32_t>(std::min<uint64_t>(limit, UINT32_MAX - 1u))));
}

uint32_t EmitLoopGuardCondition(EmitterState& state, uint32_t condition, bool exit_on_true) {
	const auto tripped = EmitLoopGuardTripped(state, 1);
	return exit_on_true ? Binary(state, spv::OpLogicalOr, TypeBool(state), condition, tripped)
	                    : Binary(state, spv::OpLogicalAnd, TypeBool(state), condition,
	                             Unary(state, spv::OpLogicalNot, TypeBool(state), tripped));
}

void EmitLoopGuardLog(EmitterState& state, const IR::Block* header, uint32_t factor) {
	if (state.loop_guard_log_address == 0) {
		return;
	}
	const auto tripped = EmitLoopGuardTripped(state, factor);
	const auto fresh   = Binary(state, spv::OpIEqual, TypeBool(state), LoadU32(state, state.loop_guard_logged),
	                            ConstantU32(state, 0));
	const auto log_now = Binary(state, spv::OpLogicalAnd, TypeBool(state), tripped, fresh);
	// The values to record, taken before the selection (the header dominates this block).
	std::array<uint32_t, LoopGuard::kPhis> values {};
	values.fill(0);
	uint32_t phis = 0;
	if (header != nullptr) {
		for (const auto& inst: *header) {
			if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
				break;
			}
			if (phis < values.size() && inst.GetType() == IR::Type::U32 && inst.Definition(0) != 0) {
				values[phis++] = inst.Definition(0);
			}
		}
	}
	EmitIfCondition(state, log_now, [&] {
		state.builder.AddFunction(spv::OpStore, state.loop_guard_logged, ConstantU32(state, 1));
		const auto log  = GuardLogWords(state);
		const auto slot = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAtomicIAdd, TypeU32(state), slot, GuardLogWord(state, log, ConstantU32(state, 0)),
		                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, spv::MemorySemanticsMaskNone),
		                          ConstantU32(state, 1));
		const auto room = Binary(state, spv::OpULessThan, TypeBool(state), slot,
		                         ConstantU32(state, LoopGuard::kMaxEntries));
		EmitIfCondition(state, room, [&] {
			const auto base = EmitAddU32(
			    state, ConstantU32(state, LoopGuard::kHeaderDwords),
			    Binary(state, spv::OpIMul, TypeU32(state), slot, ConstantU32(state, LoopGuard::kEntryDwords)));
			std::array<uint32_t, LoopGuard::kEntryDwords> words {};
			words[0] = ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash));
			words[1] = ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash >> 32u));
			words[2] = ConstantU32(state, header != nullptr ? header->id : UINT32_MAX);
			words[3] = LoadU32(state, state.loop_guard_counter);
			for (uint32_t i = 0; i < LoopGuard::kPhis; i++) {
				words[4 + i] = values[i] != 0 ? values[i] : ConstantU32(state, 0);
			}
			for (uint32_t word = 0; word < words.size(); word++) {
				const auto pointer = GuardLogWord(state, log, EmitAddU32(state, base, ConstantU32(state, word)));
				state.builder.AddFunction(spv::OpStore, pointer, words[word], spv::MemoryAccessAlignedMask,
				                          static_cast<uint32_t>(sizeof(uint32_t)));
			}
		});
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
