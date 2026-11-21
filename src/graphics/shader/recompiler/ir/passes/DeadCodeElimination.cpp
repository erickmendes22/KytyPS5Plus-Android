#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::IR {

void RemoveIdentities(Program& program) {
	// U59 plan-reference correctness fix (Jetsku/KytyPS5 ce0e6ef):
	// Identity nodes are removed from IR blocks, but descriptor/SRT/fill
	// plans are not IR users. Without resolving their references first,
	// a planned read can still point to freed Identity storage. This can
	// turn the shader's live material/lighting descriptors into stale data
	// after simplification, even though its Vulkan pipeline was created.
	const auto resolve_plan_value = [](Value& value) {
		if (value.IsIdentity()) value = value.Resolve();
	};
	for (auto& source: program.descriptor_sources) {
		for (auto& dword: source.dwords) resolve_plan_value(dword);
		if (source.indirect_descriptor) {
			auto& indirect = *source.indirect_descriptor;
			resolve_plan_value(indirect.key_count);
			resolve_plan_value(indirect.selector_first);
			resolve_plan_value(indirect.selector_mask);
		}
	}
	for (auto& read: program.srt_reads) resolve_plan_value(read.value);
	for (auto& block: program.control_flow) resolve_plan_value(block.condition);
	for (auto& value: program.uniform_fill.values) resolve_plan_value(value);
	resolve_plan_value(program.uniform_fill.pattern_size);
	resolve_plan_value(program.uniform_fill.element_count);
	// Resolve metadata before deleting its identity nodes; retain only folded native constants.
	for (auto& write: program.scalar_writes) {
		const auto value = write.value.Resolve();
		write.value = value.IsImmediate() && value.GetType() == Type::U32 ? value : Value {};
	}
	for (auto* block: program.blocks) {
		block->condition       = block->condition.Resolve();
		block->indirect_target = block->indirect_target.Resolve();
	}
	for (auto* block: program.blocks) {
		auto& instructions = block->Instructions();
		for (auto inst = instructions.begin(); inst != instructions.end();) {
			if (inst->GetOpcode() != ValueOpcode::Identity) {
				inst++;
				continue;
			}
			const auto replacement = inst->Arg(0);
			inst->ReplaceUsesWith(replacement, false);
			inst = instructions.erase(inst);
		}
	}
}

void EliminateDeadCode(const BlockList& blocks) {
	bool changed;
	do {
		std::vector<Inst*> pending;
		for (auto* block: blocks) {
			for (auto& inst: *block) {
				// Retained resource-planning instructions live outside the block list.
				inst.live = inst.MayHaveSideEffects() || std::ranges::any_of(inst.Uses(),
				    [](const Use& use) { return use.user->Parent() == nullptr; });
				if (inst.live) pending.push_back(&inst);
			}
		}
		while (!pending.empty()) {
			const auto* inst = pending.back();
			pending.pop_back();
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
				auto* input = inst->Arg(arg).TryInstruction();
				if (input != nullptr && input->Parent() != nullptr && !input->live) {
					input->live = true;
					pending.push_back(input);
				}
			}
		}
		// Unlink every dead operand before freeing nodes in a dead Phi cycle.
		for (auto* block: blocks)
			for (auto& inst: *block)
				if (!inst.live) inst.Invalidate();
		for (auto* block: blocks)
			std::erase_if(block->Instructions(), [](const Inst& inst) { return !inst.live; });

		changed = false;
		for (auto* block: blocks) {
			const auto& term = block->terminator;
			if (term.kind != CFG::TerminatorKind::ConditionalBranch || term.loop_header ||
			    block->condition.IsImmediate()) continue;
			for (const bool take_true: {false, true}) {
				const auto* empty = take_true ? term.false_block : term.true_block;
				const auto* merge = take_true ? term.true_block : term.false_block;
				if (empty == nullptr || merge == nullptr || !empty->empty() ||
				    empty->terminator.loop_header || merge->terminator.loop_header ||
				    empty->terminator.kind != CFG::TerminatorKind::Branch ||
				    empty->terminator.true_block != merge ||
				    std::ranges::any_of(*merge, [](const Inst& inst) {
					    return inst.GetOpcode() == ValueOpcode::Phi;
				    })) continue;
				// Keep the CFG and SSA edges intact; only the empty detour's predicate is dead.
				for (auto& inst: *block) {
					if (inst.GetOpcode() == ValueOpcode::Reference &&
					    inst.Arg(0).Resolve() == block->condition.Resolve())
						inst.SetArg(0, Value(take_true));
				}
				block->condition = Value(take_true);
				changed = true;
				break;
			}
		}
	} while (changed);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
