#include "graphics/shader/recompiler/ir/passes/DeadLaneFold.h"

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool IsExecMerge(const Inst& inst) {
	return inst.GetOpcode() == ValueOpcode::SelectU32 &&
	       inst.Flags<SelectOrigin>() == SelectOrigin::ExecMerge;
}

// The EXEC operand of an operation that writes memory: its last U1 argument.
bool SideEffectExec(const Inst& inst, Value& exec) {
	const auto op = inst.GetOpcode();
	if (!HasSideEffects(op) || inst.NumArgs() == 0) return false;
	switch (op) {
		case ValueOpcode::SetExec:
		case ValueOpcode::SetVcc:
		case ValueOpcode::SetScc:
		case ValueOpcode::Reference:
		case ValueOpcode::SetThreadBitScalarRegister:
		case ValueOpcode::SetScalarMaskTag:
		case ValueOpcode::SetGotoVariable: return false;
		default: break;
	}
	const size_t last = inst.NumArgs() - 1;
	if (ArgTypeOf(op, last) != Type::U1) return false;
	exec = inst.Arg(last);
	return true;
}

// Lane operations whose EXEC operand must not let them see a dead lane: they read other lanes or
// count the active ones.
bool LaneOpExec(const Inst& inst, Value& exec) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::Ballot: exec = inst.Arg(0); return true;
		case ValueOpcode::ReadFirstLane: exec = inst.Arg(1); return true;
		case ValueOpcode::DppMoveU32: exec = inst.Arg(1); return true;
		case ValueOpcode::DppUpdateU32: exec = inst.Arg(2); return true;
		default: return false;
	}
}

// Operands whose values in other (possibly dead) lanes the operation can read.
void CrossLaneSources(const Inst& inst, std::vector<Value>& out) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::ReadLane: // unless its lane comes from a P ballot: see ActiveLane
		case ValueOpcode::SubgroupUMinU32: out.push_back(inst.Arg(0)); break;
		case ValueOpcode::WriteLane: out.push_back(inst.Arg(0)); break; // the other lanes pass through
		case ValueOpcode::DppMoveU32:
			if (inst.Flags<DppMoveFlags>().fetch_inactive) out.push_back(inst.Arg(0));
			break;
		case ValueOpcode::DppUpdateU32:
			if (inst.Flags<DppMoveFlags>().fetch_inactive) out.push_back(inst.Arg(0));
			break;
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::PermuteU32:
		case ValueOpcode::BpermuteU32:
		case ValueOpcode::SwizzleU32:
			for (size_t i = 0; i < inst.NumArgs(); i++) {
				if (inst.Arg(i).GetType() == Type::U32) out.push_back(inst.Arg(i));
			}
			break;
		default: break;
	}
}

class Analysis {
public:
	explicit Analysis(Program& program): m_program(program) {}

	DeadLaneFoldStats Run() {
		DeadLaneFoldStats stats;
		Number();
		// Candidate P: the most frequent ExecMerge condition first.
		std::unordered_map<Inst*, uint32_t> frequency;
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (!IsExecMerge(inst)) continue;
				if (auto* p = inst.Arg(0).Resolve().TryInstruction(); p != nullptr) frequency[p]++;
			}
		}
		std::vector<std::pair<uint32_t, Inst*>> order;
		for (const auto& [p, n]: frequency) order.emplace_back(n, p);
		std::ranges::sort(order, [this](const auto& a, const auto& b) {
			return a.first != b.first ? a.first > b.first : m_index.at(a.second) < m_index.at(b.second);
		});
		stats.reject = "no ExecMerge select";
		for (const auto& [count, p]: order) {
			if (count < 2) break; // a lone select is not worth the analysis
			const char* reason = Monotone(p);
			if (reason != nullptr) {
				stats.reject = reason;
				continue;
			}
			return Fold(p);
		}
		return stats;
	}

private:
	void Number() {
		uint32_t index = 0;
		for (size_t b = 0; b < m_program.blocks.size(); b++) {
			auto* block = m_program.blocks[b];
			m_block_index[block] = static_cast<uint32_t>(b);
			for (auto& inst: *block) m_index[&inst] = index++;
		}
		// Loop bodies: from a loop header up to (excluding) its merge block.
		for (auto* block: m_program.blocks) {
			const auto& term = block->terminator;
			if (term.loop_header && term.merge_block != nullptr) {
				m_loops.emplace_back(m_block_index[block], m_block_index[term.merge_block]);
			}
		}
	}

	[[nodiscard]] bool InLoop(const Inst* inst) const {
		const auto b = m_block_index.at(inst->Parent());
		return std::ranges::any_of(m_loops, [b](const auto& loop) {
			return b >= loop.first && b < loop.second;
		});
	}

	// Whether `value` is false in every lane where P is false.
	bool Implies(Value value, Inst* p) {
		value = value.Resolve();
		if (value.IsImmediate()) return value.GetType() == Type::U1 && !value.U1();
		auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (inst == p) return true;
		if (const auto it = m_implies.find(inst); it != m_implies.end()) return it->second;
		m_implies[inst] = true; // optimistic for phi cycles (loop-carried masks)
		bool result     = false;
		switch (inst->GetOpcode()) {
			case ValueOpcode::LogicalAnd: result = Implies(inst->Arg(0), p) || Implies(inst->Arg(1), p); break;
			case ValueOpcode::LogicalOr: result = Implies(inst->Arg(0), p) && Implies(inst->Arg(1), p); break;
			case ValueOpcode::SelectU1: result = Implies(inst->Arg(1), p) && Implies(inst->Arg(2), p); break;
			case ValueOpcode::Phi: {
				result = true;
				for (size_t i = 0; i < inst->NumArgs() && result; i++) result = Implies(inst->Arg(i), p);
				break;
			}
			default: result = false; break;
		}
		m_implies[inst] = result;
		return result;
	}

	// One evaluation step from the cached operand values (used to settle phi cycles).
	bool Step(Inst* inst, Inst* p) {
		const auto get = [&](Value value) {
			value = value.Resolve();
			if (value.IsImmediate()) return value.GetType() == Type::U1 && !value.U1();
			auto* arg = value.TryInstruction();
			if (arg == nullptr) return false;
			if (arg == p) return true;
			const auto it = m_implies.find(arg);
			return it != m_implies.end() && it->second;
		};
		switch (inst->GetOpcode()) {
			case ValueOpcode::LogicalAnd: return get(inst->Arg(0)) || get(inst->Arg(1));
			case ValueOpcode::LogicalOr: return get(inst->Arg(0)) && get(inst->Arg(1));
			case ValueOpcode::SelectU1: return get(inst->Arg(1)) && get(inst->Arg(2));
			case ValueOpcode::Phi: {
				for (size_t i = 0; i < inst->NumArgs(); i++)
					if (!get(inst->Arg(i))) return false;
				return true;
			}
			default: return false;
		}
	}

	// The optimistic recursion can leave a value true that leaned on a phi later found false:
	// iterate to the greatest fixpoint.
	void Settle(Inst* p) {
		for (bool changed = true; changed;) {
			changed = false;
			for (auto& [inst, value]: m_implies) {
				if (value && inst != p && !Step(inst, p)) {
					value   = false;
					changed = true;
				}
			}
		}
	}

	const char* Scan(Inst* p) {
		const auto start = m_index.at(p);
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (m_index.at(&inst) <= start) continue;
				Value exec;
				if (IsExecMerge(inst) && !Implies(inst.Arg(0), p)) return "a later VGPR write is not under P";
				if (SideEffectExec(inst, exec) && !Implies(exec, p)) return "a later memory write is not under P";
				if (LaneOpExec(inst, exec) && !Implies(exec, p)) return "a later lane op runs outside P";
			}
		}
		return nullptr;
	}

	const char* Monotone(Inst* p) {
		m_implies.clear();
		if (InLoop(p)) return "P is defined inside a loop";
		if (const char* reason = Scan(p)) return reason;
		Settle(p);
		return Scan(p); // with the settled cache
	}

	// Values whose dead-lane contents a cross-lane read can observe: the backward closure of the
	// cross-lane sources through every per-lane computation.
	// A lane index chosen from the set bits of a ballot whose predicate implies P (the waterfall
	// pattern: FindILsb of the low / high ballot word, selected on which word is non-zero, & 63).
	// Such a lane is active, so the read never sees a dead lane.
	bool ActiveLane(Value value, Inst* p) {
		bool ballot = false;
		return ActiveLaneTerms(value, p, 0, ballot) && ballot; // a constant lane may be dead
	}

	bool ActiveLaneTerms(Value value, Inst* p, uint32_t depth, bool& ballot) {
		value = value.Resolve();
		if (value.IsImmediate()) return true;
		auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 12) return false;
		switch (inst->GetOpcode()) {
			case ValueOpcode::Ballot:
				ballot = true;
				return Implies(inst->Arg(0), p);
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::IAdd32:
			case ValueOpcode::SelectU32:
			case ValueOpcode::INotEqual32:
			case ValueOpcode::CompositeExtractU32x4:
			case ValueOpcode::FindILsb32:
				for (size_t i = 0; i < inst->NumArgs(); i++) {
					if (inst->Arg(i).GetType() == Type::U1 && inst->GetOpcode() != ValueOpcode::SelectU32) {
						continue;
					}
					if (!ActiveLaneTerms(inst->Arg(i), p, depth + 1, ballot)) return false;
				}
				return true;
			default: return false;
		}
	}

	std::unordered_set<Inst*> LaneObservable(Inst* p) {
		std::vector<Value> work;
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::ReadLane && ActiveLane(inst.Arg(1), p)) {
					m_active_lane_reads++;
					continue;
				}
				CrossLaneSources(inst, work);
			}
		}
		// A kept ExecMerge select gives a dead lane its previous value, so only that operand's
		// dead-lane value is observable through it (the written value matters in live lanes only,
		// and the fold never changes a live lane's value).
		std::unordered_set<Inst*> seen;
		while (!work.empty()) {
			auto* inst = work.back().Resolve().TryInstruction();
			work.pop_back();
			if (inst == nullptr || !seen.insert(inst).second) continue;
			if (IsExecMerge(*inst)) {
				work.push_back(inst->Arg(2));
				continue;
			}
			for (size_t i = 0; i < inst->NumArgs(); i++) work.push_back(inst->Arg(i));
		}
		return seen;
	}

	// The condition with P removed from its LogicalAnd chain, where that is a direct operand.
	static Value WithoutP(Value condition, Inst* p) {
		auto* inst = condition.Resolve().TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::LogicalAnd) return {};
		if (inst->Arg(0).Resolve().TryInstruction() == p) return inst->Arg(1);
		if (inst->Arg(1).Resolve().TryInstruction() == p) return inst->Arg(0);
		return {};
	}

	DeadLaneFoldStats Fold(Inst* p) {
		DeadLaneFoldStats stats;
		const auto observable = LaneObservable(p);
		const auto start      = m_index.at(p);
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (!IsExecMerge(inst) || m_index.at(&inst) <= start) continue;
				if (!Implies(inst.Arg(0), p)) continue;
				stats.candidates++;
				if (observable.contains(&inst)) {
					stats.kept_lane_ops++;
					continue;
				}
				if (inst.Arg(0).Resolve().TryInstruction() == p) {
					inst.ReplaceUsesWith(inst.Arg(1));
					stats.folded++;
				} else if (const auto narrowed = WithoutP(inst.Arg(0), p); !narrowed.IsEmpty()) {
					inst.SetArg(0, narrowed);
					stats.narrowed++;
				}
			}
		}
		if (stats.folded == 0 && stats.narrowed == 0) stats.reject = "every candidate reaches a cross-lane read";
		return stats;
	}

	Program&                                  m_program;
	std::unordered_map<const Inst*, uint32_t> m_index;
	std::unordered_map<const Block*, uint32_t> m_block_index;
	std::vector<std::pair<uint32_t, uint32_t>> m_loops;
	std::unordered_map<Inst*, bool>           m_implies;
	uint32_t                                  m_active_lane_reads = 0;
};

} // namespace

DeadLaneFoldStats FoldDeadLaneSelects(Program& program) {
	return Analysis(program).Run();
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
