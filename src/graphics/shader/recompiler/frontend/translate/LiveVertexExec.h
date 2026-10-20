#pragma once

#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <cstdint>
#include <optional>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// The NGG vertex prologue builds EXEC from the vertex count in s3 (ESVertCount):
//   s_sub_i32  R, 0, s3
//   s_lshr_b64 exec, -1, R        (s_lshr_b32 exec_lo in wave32)
// That EXEC is exactly the lanes holding a vertex. The host runs one invocation per vertex, so for
// every running invocation the lane bit is set: the shift's lane predicate is true (KYTY_VS_LIVE_EXEC).
// Returns the pc of the shift when the pair sits in the entry block, s3 is unwritten from the
// entry to the subtraction and R is unwritten between the two instructions; otherwise nothing.
[[nodiscard]] std::optional<uint32_t> FindLiveVertexExecPc(const Decoder::Program& decoded,
                                                           const CFG::Graph&       cfg);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
