#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_LOOPGUARDPLAN_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_LOOPGUARDPLAN_H_

#include "graphics/shader/recompiler/ir/Block.h"

#include <span>
#include <vector>

// KYTY_LOOP_GUARD (graphics/shader/loopGuard.h): where the SPIR-V emitter counts and forces the
// exit of each structured loop, from the blocks in emission (structured layout) order.
namespace Libs::Graphics::ShaderRecompiler::IR {

struct LoopGuardPoint {
	const Block* header       = nullptr; // counts one iteration (after its phis)
	const Block* guard        = nullptr; // its conditional branch is forced to the exit
	bool         exit_on_true = false;   // the true target leaves the loop
	// Where a trip is logged: the break block the exit edge enters, or the loop's merge block;
	// nullptr when neither can hold a selection (a merge block that is itself a loop header).
	const Block* log_block = nullptr;
};

struct LoopGuardPlan {
	std::vector<LoopGuardPoint> loops;     // one per guardable loop
	std::vector<const Block*>   unguarded; // loop headers without a recognisable exit branch
};

// For each loop header (terminator.loop_header) with a merge block later in `blocks`, the first
// conditional branch, looked for in the header, then its continue block, then the blocks between
// the header and its merge, whose one target leaves the loop: the merge block itself or a break
// block (an unconditional branch to the merge that is not a loop header) and whose other
// target does not.
[[nodiscard]] LoopGuardPlan PlanLoopGuards(std::span<Block* const> blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_LOOPGUARDPLAN_H_
