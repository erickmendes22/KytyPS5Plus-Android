#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ReadLaneStats {
	uint32_t rewritten_reads      = 0;
	uint32_t rewritten_reductions = 0;
};

// reductions: whether whole-wave minima may become SubgroupUMinU32, which needs native subgroup
// arithmetic (or a one-lane stage, where the reduction is the value itself).
[[nodiscard]] ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size,
                                              bool reductions = true);

} // namespace Libs::Graphics::ShaderRecompiler::IR
