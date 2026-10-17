#include "graphics/shader/recompiler/ir/passes/NativeFloat16.h"

#include <optional>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct HalfOperation {
	ValueOpcode opcode;
	Value       lhs;
	Value       rhs;
};

std::optional<ValueOpcode> NativeOpcode(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::FPAdd32: return ValueOpcode::FPAdd16;
		case ValueOpcode::FPSub32: return ValueOpcode::FPSub16;
		case ValueOpcode::FPMul32: return ValueOpcode::FPMul16;
		default: return std::nullopt;
	}
}

// The F16 operand of ConvertF32F16, or an empty value.
Value WidenedHalf(Value value) {
	const auto* inst = value.Resolve().TryInstruction();
	return inst != nullptr && inst->GetOpcode() == ValueOpcode::ConvertF32F16 ? inst->Arg(0)
	                                                                           : Value {};
}

// value = FPAdd32|FPSub32|FPMul32(ConvertF32F16(a), ConvertF32F16(b)) used only by the rounding
// consumer.
std::optional<HalfOperation> MatchHalfOperation(Value value) {
	const auto* inst = value.Resolve().TryInstruction();
	if (inst == nullptr || inst->UseCount() != 1u) return std::nullopt;
	const auto native = NativeOpcode(inst->GetOpcode());
	if (!native) return std::nullopt;
	const auto lhs = WidenedHalf(inst->Arg(0));
	const auto rhs = WidenedHalf(inst->Arg(1));
	if (lhs.IsEmpty() || rhs.IsEmpty()) return std::nullopt;
	return HalfOperation {*native, lhs, rhs};
}

Value EmitHalf(Block& block, Block::iterator at, Value f32, const std::optional<HalfOperation>& op) {
	if (op) return Value(&*block.PrependNewInst(at, op->opcode, {op->lhs, op->rhs}));
	return Value(&*block.PrependNewInst(at, ValueOpcode::ConvertF16F32, {f32}));
}

Value HalfBits(Block& block, Block::iterator at, Value half) {
	const auto bits = Value(&*block.PrependNewInst(at, ValueOpcode::BitCastU16F16, {half}));
	return Value(&*block.PrependNewInst(at, ValueOpcode::ConvertU32U16, {bits}));
}

} // namespace

NativeFloat16Stats LowerNativeFloat16(const BlockList& blocks) {
	NativeFloat16Stats stats;
	for (auto* block: blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() == ValueOpcode::ConvertF16F32) {
				const auto op = MatchHalfOperation(inst.Arg(0));
				if (!op) continue;
				inst.ReplaceUsesWith(EmitHalf(*block, it, {}, op));
				++stats.scalar;
				continue;
			}
			if (inst.GetOpcode() != ValueOpcode::PackHalf2x16) continue;
			const auto* pair = inst.Arg(0).Resolve().TryInstruction();
			if (pair == nullptr || pair->GetOpcode() != ValueOpcode::CompositeConstructF32x2 ||
			    pair->UseCount() != 1u) {
				continue;
			}
			const auto low  = MatchHalfOperation(pair->Arg(0));
			const auto high = MatchHalfOperation(pair->Arg(1));
			if (!low && !high) continue;
			const auto low_bits  = HalfBits(*block, it, EmitHalf(*block, it, pair->Arg(0), low));
			const auto high_bits = HalfBits(*block, it, EmitHalf(*block, it, pair->Arg(1), high));
			const auto shifted   = Value(&*block->PrependNewInst(
			    it, ValueOpcode::ShiftLeftLogical32, {high_bits, Value(16u)}));
			inst.ReplaceUsesWith(
			    Value(&*block->PrependNewInst(it, ValueOpcode::BitwiseOr32, {low_bits, shifted})));
			stats.packed += static_cast<uint32_t>(low.has_value()) + static_cast<uint32_t>(high.has_value());
		}
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
