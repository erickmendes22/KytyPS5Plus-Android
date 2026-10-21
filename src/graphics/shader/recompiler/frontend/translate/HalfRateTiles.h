#pragma once

// The translator side of the half-rate tile patch (graphics/shader/halfRateTiles.h). The plan
// holds copies of the guest instructions the patch replays: the light-culling item code (4 passes
// per thread), and at the end of the kernel the descriptor loads, the depth and G-buffer loads,
// and the two output stores. It is built only when every patch point decodes to the expected
// instruction.

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/Block.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

struct HalfRatePlan {
	bool                              half           = false; // false: light-list capture only
	bool                              capture        = false; // KYTY_RT_HALF_CHECK log
	uint32_t                          capture_region = 0;
	uint32_t                          lds_base_bytes = 0;     // the patch's sample arrays
	uint32_t                          mask_base_bytes = 0;    // the check's stress-culling mask
	uint8_t                           fill_mode      = 0;     // HalfRateTiles::kFill*
	std::vector<Decoder::Instruction> cull;                   // item code without its branches
	// Culling passes 1..3 run in their own blocks, each skipped (uniform branch) when 64k >= the
	// item count: pass_blocks[k-1] holds pass k, tail_blocks[k-1] follows it. TranslateProgram
	// splits the CFG block holding kPcCullBegin this way; its terminator moves to tail_blocks[2].
	std::array<IR::Block*, 3>         pass_blocks {};
	std::array<IR::Block*, 3>         tail_blocks {};
	Decoder::Instruction              depth_desc;
	Decoder::Instruction              gbuffer_desc;
	Decoder::Instruction              depth_load;
	Decoder::Instruction              gbuffer_load;
	Decoder::Instruction              color_desc;
	Decoder::Instruction              color_store;
	Decoder::Instruction              luma_desc;
	Decoder::Instruction              luma_store;
};

// variant: HalfRateTiles::kVariantHalf or kVariantCaptureOnly; lds_dwords: the program's LDS size
// (including the patch's words for the half variant).
std::optional<HalfRatePlan> MakeHalfRatePlan(const Decoder::Program& decoded, uint8_t variant,
                                             uint32_t lds_dwords, uint8_t fill_mode, bool capture,
                                             std::string* reason);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
