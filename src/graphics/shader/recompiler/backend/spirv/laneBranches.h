#pragma once

#include <array>
#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct Program;
} // namespace Libs::Graphics::ShaderRecompiler::IR

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Constructs that make a per-lane branch differ from the guest's whole-wave skip. A region with
// one of them keeps its vote branch (and EXEC selects) unless the bit is set in
// `allowed_hazards` (KYTY_LANE_BRANCHES_HAZARDS, a diagnostic for bisecting).
enum LaneBranchHazard : uint32_t {
	// A pixel export with the valid-mask bit (EXP ... vm) records EXEC for every lane, including
	// the ones EXEC has off: lanes that skip it would keep an undefined valid mask (not killed).
	LaneBranchHazardValidMask = 1u << 0u,
	// S_ENDPGM inside the region ends the whole guest wave, also the lanes waiting at the merge.
	LaneBranchHazardReturn = 1u << 1u,
	// A vote (S_CBRANCH_VCCZ/EXECZ...) or ballot / READFIRSTLANE inside the region over a mask
	// that is not within the region predicate: the guest counts lanes the per-lane branch skips.
	LaneBranchHazardWideMask = 1u << 2u,
	// Pixel IMAGE_GET_LOD: implicit derivatives need the quad neighbours.
	LaneBranchHazardDerivative = 1u << 3u,
	LaneBranchHazardCount      = 4u,
};

struct LaneBranchStats {
	uint32_t converted = 0;
	// Regions that passed every other check and have this hazard (index = bit number); they are
	// converted only when the bit is allowed.
	std::array<uint32_t, LaneBranchHazardCount> hazard_regions {};
};

// ShaderDeviceFeatures::lane_exec_branches. An S_CBRANCH_EXECZ-style branch skips a region only
// when every lane has EXEC off (a subgroup vote); the lanes left in the region then write their
// results through EXEC selects. When the region has no cross-lane operation and every value it
// hands to the merge falls back to the pre-region value for inactive lanes, a per-lane branch on
// the EXEC predicate gives the same results: the inactive lanes take the skip edge. The driver
// can then drop the selects (their predicate is true inside the branch) and skip the work.
// Structured functions only.
LaneBranchStats ConvertExecVoteBranches(IR::Program& program, uint32_t allowed_hazards = 0u);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
