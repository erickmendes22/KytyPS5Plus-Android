#pragma once

// The translator side of the half-rate tile patch (graphics/shader/halfRateTiles.h): the plan
// holds copies of the guest instructions the patch replays at the end of the kernel (descriptor
// loads, the depth and G-buffer loads, the two output stores). It is built only when every
// patch point decodes to the expected instruction.

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

struct HalfRatePlan {
	uint32_t             lds_base_bytes = 0;
	Decoder::Instruction depth_desc;
	Decoder::Instruction gbuffer_desc;
	Decoder::Instruction depth_load;
	Decoder::Instruction gbuffer_load;
	Decoder::Instruction color_desc;
	Decoder::Instruction color_store;
	Decoder::Instruction luma_desc;
	Decoder::Instruction luma_store;
};

// lds_dwords: the program's LDS size including the patch's words.
std::optional<HalfRatePlan> MakeHalfRatePlan(const Decoder::Program& decoded, uint32_t lds_dwords,
                                             std::string* reason);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
