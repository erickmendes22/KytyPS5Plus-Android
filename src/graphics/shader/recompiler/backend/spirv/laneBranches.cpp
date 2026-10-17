#include "graphics/shader/recompiler/backend/spirv/laneBranches.h"

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

using IR::Block;
using IR::Inst;
using IR::Value;
using IR::ValueOpcode;

Inst* InstOf(Value value) {
	return value.IsEmpty() ? nullptr : value.Resolve().TryInstruction();
}

bool IsSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU32 || op == ValueOpcode::SelectF32 ||
	       op == ValueOpcode::SelectU1;
}

// Operations whose result for one lane depends on other lanes' registers or on lanes the
// region would no longer run (explicit lane reads, DPP/swizzles, implicit derivatives).
bool CrossesLanes(const IR::Program& program, const Inst& inst) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::ReadLane:
		case ValueOpcode::WriteLane:
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::SwizzleU32:
		case ValueOpcode::SubgroupUMinU32:
		case ValueOpcode::Barrier: return true;
		case ValueOpcode::ImageSampleRaw: {
			if (program.stage != ShaderType::Pixel) return false;
			const auto& mem   = program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
			const auto  flags = mem.image_sample_flags;
			return (flags & (Decoder::ImageSampleFlagDerivative | Decoder::ImageSampleFlagLod |
			                 Decoder::ImageSampleFlagLevelZero)) == 0u;
		}
		default: return false;
	}
}

class Converter {
public:
	explicit Converter(IR::Program& program): m_program(program) {}

	uint32_t Run() {
		uint32_t converted = 0;
		for (auto* block: m_program.blocks) {
			converted += TryConvert(*block) ? 1u : 0u;
		}
		return converted;
	}

private:
	// The per-lane EXEC predicate an exec-zero/nonzero vote branch tests, or nullptr.
	struct VoteBranch {
		Value predicate;     // P: the lane is active in the guarded region
		Value not_predicate; // !P when it already exists
		Block* region = nullptr;
	};

	bool ResolveVote(const Block& block, VoteBranch& out) const {
		const auto& term = block.terminator;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch || term.loop_header ||
		    term.merge_block == nullptr || term.true_block == nullptr ||
		    term.false_block == nullptr || block.condition.IsEmpty()) {
			return false;
		}
		bool  negated = false;
		Inst* inst    = InstOf(block.condition);
		while (inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalNot) {
			negated = !negated;
			inst    = InstOf(inst->Arg(0));
		}
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::ConditionRef) return false;
		const auto kind = inst->Flags<CFG::BranchCondition>();
		const auto arg  = inst->Arg(0).Resolve();
		// The vote is true when EXEC is zero (ExecZero) or nonzero (ExecNonZero).
		bool vote_means_active;
		if (kind == CFG::BranchCondition::ExecZero) {
			const auto* not_inst = InstOf(arg);
			if (not_inst == nullptr || not_inst->GetOpcode() != ValueOpcode::LogicalNot) return false;
			out.predicate     = not_inst->Arg(0).Resolve();
			out.not_predicate = arg;
			vote_means_active = false;
		} else if (kind == CFG::BranchCondition::ExecNonZero) {
			out.predicate     = arg;
			vote_means_active = true;
		} else {
			return false;
		}
		// The condition is true when (vote == vote_means_active) != negated.
		const bool true_is_active = vote_means_active != negated;
		out.region = true_is_active ? term.true_block : term.false_block;
		Block* skip = true_is_active ? term.false_block : term.true_block;
		return skip == term.merge_block && out.region != term.merge_block;
	}

	// Blocks reachable from the region entry before the merge; false if the region has another
	// exit or another entry.
	bool CollectRegion(const Block& branch, Block* entry, const Block* merge,
	                   std::unordered_set<const Block*>& region) const {
		std::vector<Block*> pending {entry};
		region.insert(entry);
		while (!pending.empty()) {
			auto* block = pending.back();
			pending.pop_back();
			for (auto* succ: block->ImmSuccessors()) {
				if (succ == merge || region.contains(succ)) continue;
				if (succ == &branch) return false;
				region.insert(succ);
				pending.push_back(succ);
			}
		}
		for (const auto* block: region) {
			for (const auto* succ: block->ImmSuccessors()) {
				if (succ != merge && !region.contains(succ)) return false;
			}
			for (const auto* pred: block->ImmPredecessors()) {
				if (!region.contains(pred) && !(block == entry && pred == &branch)) return false;
			}
		}
		for (const auto* pred: merge->ImmPredecessors()) {
			if (pred != &branch && !region.contains(pred)) return false;
		}
		return true;
	}

	static bool Implies(Value condition, Value predicate, int depth = 0) {
		condition = condition.Resolve();
		if (condition == predicate) return true;
		if (depth > 16) return false;
		const auto* inst = InstOf(condition);
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::LogicalAnd) return false;
		return Implies(inst->Arg(0), predicate, depth + 1) ||
		       Implies(inst->Arg(1), predicate, depth + 1);
	}

	// A store, atomic or export in the region must be gated (last operand) by EXEC within P: a
	// write under a wider EXEC restored inside the region would lose its inactive lanes.
	static bool SideEffectGuarded(const Inst& inst, Value predicate) {
		const auto op = inst.GetOpcode();
		if (!inst.MayHaveSideEffects() || op == ValueOpcode::Reference ||
		    op == ValueOpcode::ReferenceU32) {
			return true;
		}
		if (inst.NumArgs() == 0) return false;
		const auto guard = inst.Arg(inst.NumArgs() - 1).Resolve();
		return guard.GetType() == IR::Type::U1 && Implies(guard, predicate);
	}

	// Lanes outside the predicate leave the region with `skip`: the region value is `skip`
	// itself or a chain of predicate-gated selects / region Phis that falls back to it.
	bool Guarded(Value value, Value skip, Value predicate,
	             const std::unordered_set<const Block*>& region,
	             std::unordered_set<const Inst*>& visiting) const {
		value = value.Resolve();
		if (value == skip.Resolve()) return true;
		auto* inst = InstOf(value);
		if (inst == nullptr || !region.contains(inst->Parent())) return false;
		const auto op = inst->GetOpcode();
		if (IsSelect(op)) {
			return Implies(inst->Arg(0), predicate) &&
			       Guarded(inst->Arg(2), skip, predicate, region, visiting);
		}
		// A narrower EXEC (P && x) is false for the inactive lanes, like a skip value that
		// implies P.
		if (op == ValueOpcode::LogicalAnd && Implies(value, predicate) &&
		    Implies(skip, predicate)) {
			return true;
		}
		if (op == ValueOpcode::Phi) {
			if (!visiting.insert(inst).second) return true;
			for (size_t i = 0; i < inst->NumArgs(); i++) {
				if (!Guarded(inst->Arg(i), skip, predicate, region, visiting)) return false;
			}
			return true;
		}
		return false;
	}

	bool TryConvert(Block& branch) {
		VoteBranch vote;
		if (!ResolveVote(branch, vote)) return false;
		auto* merge = branch.terminator.merge_block;
		std::unordered_set<const Block*> region;
		if (!CollectRegion(branch, vote.region, merge, region)) return false;
		for (const auto* block: region) {
			for (const auto& inst: *block) {
				if (CrossesLanes(m_program, inst) || !SideEffectGuarded(inst, vote.predicate)) {
					return false;
				}
			}
		}
		for (const auto& inst: *merge) {
			if (inst.GetOpcode() != ValueOpcode::Phi) continue;
			Value skip;
			for (size_t i = 0; i < inst.NumArgs(); i++) {
				if (inst.PhiBlock(i) == &branch) skip = inst.Arg(i);
			}
			if (skip.IsEmpty()) return false;
			for (size_t i = 0; i < inst.NumArgs(); i++) {
				if (inst.PhiBlock(i) == &branch) continue;
				std::unordered_set<const Inst*> visiting;
				if (!Guarded(inst.Arg(i), skip, vote.predicate, region, visiting)) return false;
			}
		}
		const bool region_is_true = branch.terminator.true_block == vote.region;
		if (region_is_true) {
			branch.condition = vote.predicate;
		} else {
			if (vote.not_predicate.IsEmpty()) {
				vote.not_predicate =
				    Value(&branch.AppendNewInst(ValueOpcode::LogicalNot, {vote.predicate}));
			}
			branch.condition = vote.not_predicate;
		}
		return true;
	}

	IR::Program& m_program;
};

} // namespace

uint32_t ConvertExecVoteBranches(IR::Program& program) {
	if (program.dispatcher_fallback) return 0;
	return Converter(program).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
