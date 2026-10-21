#include "graphics/shader/recompiler/ir/passes/LoopGuardPlan.h"

#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

// Whether branching to `target` leaves the loop that merges at `merge`, and where to log it.
bool Exits(const Block* target, const Block* merge, const Block** log_block) {
	if (target == nullptr) {
		return false;
	}
	if (target == merge) {
		*log_block = merge->terminator.loop_header ? nullptr : merge;
		return true;
	}
	const auto& term = target->terminator;
	if (term.kind == CFG::TerminatorKind::Branch && term.true_block == merge && !term.loop_header) {
		*log_block = target;
		return true;
	}
	return false;
}

} // namespace

LoopGuardPlan PlanLoopGuards(std::span<Block* const> blocks) {
	LoopGuardPlan                            plan;
	std::unordered_map<const Block*, size_t> index;
	for (size_t i = 0; i < blocks.size(); i++) {
		index.emplace(blocks[i], i);
	}
	for (size_t h = 0; h < blocks.size(); h++) {
		const Block* header = blocks[h];
		const auto&  term   = header->terminator;
		if (!term.loop_header) {
			continue;
		}
		const auto merge = index.find(term.merge_block);
		if (term.merge_block == nullptr || merge == index.end() || merge->second <= h) {
			plan.unguarded.push_back(header);
			continue;
		}
		std::vector<const Block*> candidates {header};
		if (term.continue_block != nullptr && term.continue_block != header) {
			candidates.push_back(term.continue_block);
		}
		for (size_t i = h + 1; i < merge->second; i++) {
			if (blocks[i] != term.continue_block) {
				candidates.push_back(blocks[i]);
			}
		}
		bool found = false;
		for (const Block* block: candidates) {
			const auto& branch = block->terminator;
			if (branch.kind != CFG::TerminatorKind::ConditionalBranch || block->condition.IsEmpty()) {
				continue;
			}
			const Block* log_true  = nullptr;
			const Block* log_false = nullptr;
			const bool   true_out  = Exits(branch.true_block, term.merge_block, &log_true);
			const bool   false_out = Exits(branch.false_block, term.merge_block, &log_false);
			if (true_out == false_out) {
				continue;
			}
			plan.loops.push_back({.header       = header,
			                      .guard        = block,
			                      .exit_on_true = true_out,
			                      .log_block    = true_out ? log_true : log_false});
			found = true;
			break;
		}
		if (!found) {
			plan.unguarded.push_back(header);
		}
	}
	return plan;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
