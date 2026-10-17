#include "graphics/shader/recompiler/backend/spirv/laneBranches.h"

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include "common/logging/log.h"

#include <cinttypes>
#include <cstdlib>
#include <string>
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
		case ValueOpcode::PermuteU32:
		case ValueOpcode::BpermuteU32:
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
	Converter(IR::Program& program, uint32_t allowed_hazards)
	    : m_program(program), m_allowed_hazards(allowed_hazards) {}

	void Run() {
		for (auto* block: m_program.blocks) {
			m_stats.converted += TryConvert(*block) ? 1u : 0u;
		}
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

	// Whether `condition` is false wherever `predicate` is: a chain of ANDs containing P, ORs and
	// selects whose every leaf implies P, or a region Phi whose incoming values all do (a Phi
	// already being proved is assumed: region Phis only carry values of this region visit, so
	// induction over the iterations holds; a Phi outside the region may carry an older P).
	bool Implies(Value condition, Value predicate) const {
		std::unordered_set<const Inst*> proving;
		return Implies(condition, predicate, proving, 0);
	}

	bool Implies(Value condition, Value predicate, std::unordered_set<const Inst*>& proving,
	             int depth) const {
		condition = condition.Resolve();
		if (condition == predicate) return true;
		if (condition.IsImmediate()) {
			return condition.GetType() == IR::Type::U1 && !condition.U1();
		}
		if (depth > 32) return false;
		auto* inst = InstOf(condition);
		if (inst == nullptr) return false;
		switch (inst->GetOpcode()) {
			case ValueOpcode::LogicalAnd:
				return Implies(inst->Arg(0), predicate, proving, depth + 1) ||
				       Implies(inst->Arg(1), predicate, proving, depth + 1);
			case ValueOpcode::LogicalOr:
				return Implies(inst->Arg(0), predicate, proving, depth + 1) &&
				       Implies(inst->Arg(1), predicate, proving, depth + 1);
			case ValueOpcode::SelectU1:
				return Implies(inst->Arg(1), predicate, proving, depth + 1) &&
				       Implies(inst->Arg(2), predicate, proving, depth + 1);
			case ValueOpcode::Phi: {
				if (m_region == nullptr || !m_region->contains(inst->Parent())) return false;
				if (proving.contains(inst)) return true;
				proving.insert(inst);
				bool all = inst->NumArgs() != 0;
				for (size_t i = 0; all && i < inst->NumArgs(); i++) {
					all = Implies(inst->Arg(i), predicate, proving, depth + 1);
				}
				proving.erase(inst);
				return all;
			}
			default: return false;
		}
	}

	// A store, atomic or export in the region must be gated (last operand) by EXEC within P: a
	// write under a wider EXEC restored inside the region would lose its inactive lanes.
	bool SideEffectGuarded(const Inst& inst, Value predicate) const {
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

	// Whether a lane-mask operand (vote, ballot, READFIRSTLANE) only counts lanes within P. A
	// vote on a zero kind receives the inverted mask.
	bool MaskWithin(const Inst& inst, Value predicate) const {
		switch (inst.GetOpcode()) {
			case ValueOpcode::Ballot: return Implies(inst.Arg(0), predicate);
			case ValueOpcode::ReadFirstLane: return Implies(inst.Arg(1), predicate);
			case ValueOpcode::ConditionRef: {
				const auto kind = inst.Flags<CFG::BranchCondition>();
				if (kind == CFG::BranchCondition::ExecNonZero ||
				    kind == CFG::BranchCondition::VccNonZero) {
					return Implies(inst.Arg(0), predicate);
				}
				if (kind == CFG::BranchCondition::ExecZero || kind == CFG::BranchCondition::VccZero) {
					const auto* not_inst = InstOf(inst.Arg(0));
					return not_inst != nullptr && not_inst->GetOpcode() == ValueOpcode::LogicalNot &&
					       Implies(not_inst->Arg(0), predicate);
				}
				// SCC and scalar-instruction conditions are wave-uniform values; masks they
				// derive from ballots inside the region are checked at the ballot.
				return true;
			}
			default: return true;
		}
	}

	// Hazard bits of one region instruction (0 when lane-local), or UINT32_MAX for a construct
	// that can never become a per-lane branch.
	uint32_t InstHazards(const Inst& inst, Value predicate) const {
		if (CrossesLanes(m_program, inst) || !SideEffectGuarded(inst, predicate)) return UINT32_MAX;
		const bool pixel = m_program.stage == ShaderType::Pixel;
		uint32_t   hazards = 0;
		switch (inst.GetOpcode()) {
			case ValueOpcode::SetAttribute:
				if (pixel && m_program.export_info.at(inst.Flags<IR::ExportFlags>().index).vm) {
					hazards |= LaneBranchHazardValidMask;
				}
				break;
			case ValueOpcode::ImageQueryLod:
				if (pixel) hazards |= LaneBranchHazardDerivative;
				break;
			case ValueOpcode::Ballot:
			case ValueOpcode::ReadFirstLane:
			case ValueOpcode::ConditionRef:
				if (!MaskWithin(inst, predicate)) hazards |= LaneBranchHazardWideMask;
				break;
			default: break;
		}
		return hazards;
	}

	// KYTY_LANE_BRANCHES_LOG=1: one line per converted region (diagnostic).
	static bool LogRegions() {
		static const bool enabled = [] {
			const char* value = std::getenv("KYTY_LANE_BRANCHES_LOG");
			return value != nullptr && value[0] != '\0' && value[0] != '0';
		}();
		return enabled;
	}

	void LogRegion(const Block& branch, const Block& merge, const VoteBranch& vote,
	               const std::unordered_set<const Block*>& region) const {
		std::string blocks;
		uint32_t    loops = 0, votes = 0, ballots = 0, samples = 0, loads = 0, writes = 0, phis = 0;
		for (const auto* block: m_program.blocks) {
			if (!region.contains(block)) continue;
			blocks += " " + std::to_string(block->id);
			loops += block->terminator.loop_header ? 1u : 0u;
			for (const auto& inst: *block) {
				const auto op = inst.GetOpcode();
				votes += op == ValueOpcode::ConditionRef ? 1u : 0u;
				ballots += op == ValueOpcode::Ballot || op == ValueOpcode::ReadFirstLane ? 1u : 0u;
				samples += op == ValueOpcode::ImageSampleRaw ? 1u : 0u;
				loads += IR::BufferAccessOf(op) == IR::BufferAccess::Read ? 1u : 0u;
				writes += inst.MayHaveSideEffects() && op != ValueOpcode::Reference &&
				                  op != ValueOpcode::ReferenceU32
				              ? 1u
				              : 0u;
				phis += op == ValueOpcode::Phi ? 1u : 0u;
			}
		}
		uint32_t merge_phis = 0;
		for (const auto& inst: merge) merge_phis += inst.GetOpcode() == ValueOpcode::Phi ? 1u : 0u;
		const auto* predicate = InstOf(vote.predicate);
		LOGF("lane branch hash=0x%016" PRIx64 " #%u branch=%u merge=%u pred=%s blocks=[%s ] loops=%u "
		     "votes=%u ballots=%u samples=%u loads=%u writes=%u phis=%u merge_phis=%u\n",
		     m_program.shader_hash, m_stats.converted, branch.id, merge.id,
		     predicate != nullptr ? std::string(IR::ValueOpcodeName(predicate->GetOpcode())).c_str()
		                          : "imm",
		     blocks.c_str(), loops, votes, ballots, samples, loads, writes, phis, merge_phis);
	}

	bool TryConvert(Block& branch) {
		VoteBranch vote;
		if (!ResolveVote(branch, vote)) return false;
		auto* merge = branch.terminator.merge_block;
		std::unordered_set<const Block*> region;
		if (!CollectRegion(branch, vote.region, merge, region)) return false;
		m_region = &region;
		const bool convertible = Convertible(branch, *merge, vote, region);
		m_region = nullptr;
		if (!convertible) return false;
		if (LogRegions()) LogRegion(branch, *merge, vote, region);
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

	bool Convertible(const Block& branch, const Block& merge, const VoteBranch& vote,
	                 const std::unordered_set<const Block*>& region) {
		uint32_t hazards = 0;
		for (const auto* block: region) {
			const auto kind = block->terminator.kind;
			if (kind == CFG::TerminatorKind::Return) {
				hazards |= LaneBranchHazardReturn;
			} else if (kind != CFG::TerminatorKind::Branch &&
			           kind != CFG::TerminatorKind::ConditionalBranch) {
				return false;
			}
			for (const auto& inst: *block) {
				const auto inst_hazards = InstHazards(inst, vote.predicate);
				if (inst_hazards == UINT32_MAX) return false;
				hazards |= inst_hazards;
			}
		}
		for (const auto& inst: merge) {
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
		for (uint32_t bit = 0; bit < LaneBranchHazardCount; bit++) {
			if ((hazards & (1u << bit)) != 0u) m_stats.hazard_regions[bit]++;
		}
		return (hazards & ~m_allowed_hazards) == 0u;
	}

public:
	[[nodiscard]] const LaneBranchStats& Stats() const { return m_stats; }

private:
	IR::Program&                            m_program;
	uint32_t                                m_allowed_hazards = 0;
	const std::unordered_set<const Block*>* m_region          = nullptr;
	LaneBranchStats                         m_stats;
};

} // namespace

LaneBranchStats ConvertExecVoteBranches(IR::Program& program, uint32_t allowed_hazards) {
	if (program.dispatcher_fallback) return {};
	Converter converter(program, allowed_hazards);
	converter.Run();
	return converter.Stats();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
