#include "graphics/shader/recompiler/frontend/translate/LiveVertexExec.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

using Decoder::Opcode;
using Decoder::OperandKind;

constexpr uint32_t VertexCountSgpr = 3u;

// A scalar register the scan follows: an SGPR index, or VCC (either half; 64-bit writes pair them).
struct ScalarSlot {
	bool     vcc = false;
	uint32_t reg = 0;
};

std::optional<ScalarSlot> SlotOf(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case OperandKind::Sgpr: return ScalarSlot {.vcc = false, .reg = operand.reg};
		case OperandKind::VccLo:
		case OperandKind::VccHi: return ScalarSlot {.vcc = true, .reg = 0};
		default: return std::nullopt;
	}
}

// Conservative: an SGPR destination may cover max(2, data_dwords) registers (64-bit SALU, SMEM).
bool OperandWrites(const Decoder::Operand& destination, uint32_t data_dwords, ScalarSlot slot) {
	if (destination.kind == OperandKind::Sgpr) {
		const uint32_t width = std::max(2u, data_dwords);
		return !slot.vcc && slot.reg >= destination.reg && slot.reg < destination.reg + width;
	}
	if (destination.kind == OperandKind::VccLo || destination.kind == OperandKind::VccHi) {
		return slot.vcc;
	}
	return false;
}

bool Writes(const Decoder::Instruction& inst, ScalarSlot slot) {
	// VOP2 carry opcodes and VOPC write VCC implicitly; treat every VOP2/VOPC as a VCC writer.
	if (slot.vcc && (inst.family == Decoder::Family::VOP2 || inst.family == Decoder::Family::VOPC)) {
		return true;
	}
	return OperandWrites(inst.dst, inst.data_dwords, slot) ||
	       OperandWrites(inst.dst2, inst.data_dwords, slot);
}

bool IsInlineInteger(const Decoder::Operand& operand, int32_t value) {
	return operand.kind == OperandKind::IntegerInlineConstant && operand.signed_val == value;
}

bool SameSlot(const std::optional<ScalarSlot>& a, const std::optional<ScalarSlot>& b) {
	return a && b && a->vcc == b->vcc && (a->vcc || a->reg == b->reg);
}

// s_sub_i32 / s_sub_u32 R, 0, s3: R = -ESVertCount.
std::optional<ScalarSlot> NegatedCountDestination(const Decoder::Instruction& inst) {
	if (inst.opcode != Opcode::S_SUB_I32 && inst.opcode != Opcode::S_SUB_U32) {
		return std::nullopt;
	}
	if (!IsInlineInteger(inst.src0, 0) || inst.src1.kind != OperandKind::Sgpr ||
	    inst.src1.reg != VertexCountSgpr) {
		return std::nullopt;
	}
	return SlotOf(inst.dst);
}

// s_lshr_b64 exec, -1, R / s_lshr_b32 exec_lo, -1, R.
bool IsExecFromNegatedCount(const Decoder::Instruction& inst, ScalarSlot negated) {
	if (inst.opcode != Opcode::S_LSHR_B64 && inst.opcode != Opcode::S_LSHR_B32) {
		return false;
	}
	return inst.dst.kind == OperandKind::ExecLo && IsInlineInteger(inst.src0, -1) &&
	       SameSlot(SlotOf(inst.src1), negated);
}

const CFG::BasicBlock* EntryBlock(const CFG::Graph& cfg) {
	const auto found = std::ranges::find(cfg.blocks, 0u, &CFG::BasicBlock::inst_begin);
	if (found == cfg.blocks.end() || !found->predecessors.empty()) {
		return nullptr;
	}
	return &*found;
}

} // namespace

std::optional<uint32_t> FindLiveVertexExecPc(const Decoder::Program& decoded,
                                             const CFG::Graph&       cfg) {
	const auto* entry = EntryBlock(cfg);
	if (entry == nullptr || cfg.irreducible || cfg.unsupported) {
		return std::nullopt;
	}
	const ScalarSlot          count_slot {.vcc = false, .reg = VertexCountSgpr};
	bool                      count_pristine = true;
	std::optional<ScalarSlot> negated;
	const uint32_t end = std::min<uint32_t>(entry->inst_end, decoded.instructions.size());
	for (uint32_t index = entry->inst_begin; index < end; index++) {
		const auto& inst = decoded.instructions[index];
		if (negated && IsExecFromNegatedCount(inst, *negated)) {
			return inst.pc;
		}
		const auto destination =
		    count_pristine ? NegatedCountDestination(inst) : std::optional<ScalarSlot> {};
		if (negated && Writes(inst, *negated)) {
			negated.reset();
		}
		if (destination) {
			negated = destination;
		}
		if (Writes(inst, count_slot)) {
			count_pristine = false;
		}
		if (!count_pristine && !negated) {
			return std::nullopt;
		}
	}
	return std::nullopt;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
