#pragma once

// The translator side of the half-rate tile patch (graphics/shader/halfRateTiles.h). The plan
// holds copies of the guest instructions the patch replays: the light-culling item code (4 passes
// per thread), and at the end of the kernel the descriptor loads, the depth and G-buffer loads,
// and the two output stores. It is built only when every patch point decodes to the expected
// instruction.

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/Value.h"

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
	uint32_t                          check_base_bytes = 0;   // the check's stress-culling scratch
	uint8_t                           fill_mode      = 0;     // HalfRateTiles::kFill*
	std::vector<Decoder::Instruction> cull;                   // item code without its branches
	// Culling passes 1..3 run in their own blocks, each skipped (uniform branch) when 64k >= the
	// item count: pass[k-1] holds pass k, tail[k-1] follows it. TranslateProgram splits the CFG
	// block holding `pc` this way; its terminator moves to tail[2]. splits[0]: the real culling
	// (before kPcCullBegin, half only); splits[1]: the check's stress culling (after
	// kPcListBarrier, half with capture only).
	struct Split {
		uint32_t                  pc = 0;
		std::array<IR::Block*, 3> pass {};
		std::array<IR::Block*, 3> tail {};
	};
	std::array<Split, 2>              splits {};
	uint32_t                          split_count = 0;
	// rt_half=2 (HalfRateTiles::kVariantAdaptive): the re-trace loop around the lighting body.
	// pre (the CFG block holding kPcListBarrier) -> header -> body ... -> merge (all body exits) ->
	// fill (first iteration only) -> fill_merge -> latch (continue target, back edge to header or
	// exit) -> exit -> end (the s_endpgm block). TranslateProgram builds the CFG; the translator
	// fills the blocks (HalfRateLoopEnter after kPcListBarrier, HalfRateLoopExit before kPcEnd).
	struct Loop {
		IR::Block* header     = nullptr;
		IR::Block* body_entry = nullptr; // rest of the pre block's instructions and its terminator
		IR::Block* merge      = nullptr;
		IR::Block* fill       = nullptr;
		IR::Block* fill_merge = nullptr;
		IR::Block* latch      = nullptr;
		IR::Block* exit       = nullptr;
		IR::Block* end        = nullptr;
	};
	bool                              adaptive = false;
	// Registers the lighting body starts from (saved in the pre block, restored at the latch);
	// written while translating the pre block, read in the s_endpgm block.
	struct LoopSaved {
		std::vector<IR::U32> v;
		std::vector<IR::U32> s;
		IR::U32              vcc_lo {};
		IR::U32              vcc_hi {};
		IR::U1               scc {};
		IR::U32              m0 {};
	};
	static constexpr uint32_t kLoopSavedVgprs = 72;
	static constexpr uint32_t kLoopSavedSgprs = 106;
	mutable LoopSaved                 loop_saved {};
	Loop                              loop {};
	uint32_t                          adaptive_base_bytes = 0; // re-trace counter + list
	Decoder::Instruction              depth_desc;
	Decoder::Instruction              gbuffer_desc;
	Decoder::Instruction              depth_load;
	Decoder::Instruction              gbuffer_load;
	Decoder::Instruction              color_desc;
	Decoder::Instruction              color_store;
	Decoder::Instruction              luma_desc;
	Decoder::Instruction              luma_store;
	Decoder::Instruction              material_desc; // check mode: the adaptive-rate estimate
	Decoder::Instruction              material_load;
	Decoder::Instruction              albedo_desc;
	Decoder::Instruction              albedo_load;
};

// variant: HalfRateTiles::kVariantHalf or kVariantCaptureOnly; lds_dwords: the program's LDS size
// (including the patch's words for the half variant).
std::optional<HalfRatePlan> MakeHalfRatePlan(const Decoder::Program& decoded, uint8_t variant,
                                             uint32_t lds_dwords, uint8_t fill_mode, bool capture,
                                             std::string* reason);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
