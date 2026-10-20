#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct Program;

// KYTY_DEAD_LANE_FOLD (compute): an EXEC narrowing whose lanes never come back.
//
// A guest that starts with a bounds check (v_cmp; s_and_saveexec; s_cbranch_execz) turns the
// out-of-range lanes off for the rest of the program. We translate EXEC into a per-lane predicate
// P, and every VGPR write after that point into Select(P, written, previous) (an ExecMerge), so
// the dead lanes keep their old values through the whole shader: one select per write, and the
// old values stay live (registers) for nothing. Astro Bot's RT pass 6856320a has 749 such selects.
//
// When P is monotone (every EXEC-masked operation after it, i.e. every ExecMerge select and the
// EXEC operand of every side-effecting memory/image/LDS operation, uses P or a predicate that
// implies it, and P is not defined inside a loop), the dead lanes never write memory and never
// become active again, so what their registers hold is unobservable except through cross-lane
// reads. The pass then rewrites Select(P, written, previous) to `written` and drops P from
// LogicalAnd(P, x) conditions of other ExecMerge selects, except for selects whose values can
// reach a cross-lane read that may see an inactive lane (ReadLane / WriteLane chains, DPP,
// permutes, subgroup reductions), which keep their exact per-lane value. Side effects keep their
// masks. The program is left unchanged when no monotone P exists, when a Ballot, ReadFirstLane
// or DPP after P runs under a mask that does not imply P (a whole-wave region), or when nothing
// can be folded.
struct DeadLaneFoldStats {
	uint32_t folded        = 0; // selects replaced by their written value
	uint32_t narrowed      = 0; // selects whose condition lost the P term
	uint32_t kept_lane_ops = 0; // selects kept because their value reaches a cross-lane read
	uint32_t candidates    = 0; // ExecMerge selects on P-implied conditions
	const char* reject     = nullptr; // why nothing was folded (nullptr when something was)
};

DeadLaneFoldStats FoldDeadLaneSelects(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
