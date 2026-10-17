#pragma once

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct Program;
} // namespace Libs::Graphics::ShaderRecompiler::IR

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// ShaderDeviceFeatures::lane_exec_branches. An S_CBRANCH_EXECZ-style branch skips a region only
// when every lane has EXEC off (a subgroup vote); the lanes left in the region then write their
// results through EXEC selects. When the region has no cross-lane operation and every value it
// hands to the merge falls back to the pre-region value for inactive lanes, a per-lane branch on
// the EXEC predicate gives the same results: the inactive lanes take the skip edge. The driver
// can then drop the selects (their predicate is true inside the branch) and skip the work.
// Returns the number of branches converted. Structured functions only.
uint32_t ConvertExecVoteBranches(IR::Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
