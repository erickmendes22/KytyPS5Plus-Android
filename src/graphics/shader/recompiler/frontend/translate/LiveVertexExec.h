#pragma once

#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <cstdint>
#include <optional>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// The NGG vertex prologue builds EXEC from the vertex count in s3 (ESVertCount):
//   s_sub_i32  R, 0, s3
//   s_lshr_b64 exec, -1, R        (s_lshr_b32 exec_lo in wave32)
// That EXEC is the lanes holding a vertex. The host prologue writes popcount(ballot(true)) to s3, so
// with the running invocations packed at lanes 0..N-1 every running invocation's bit is set and the
// shift's lane predicate is true (KYTY_VS_LIVE_EXEC). Returns the pc of the shift when the pair sits
// in the entry block, s3 is unwritten from the entry to the subtraction, R is unwritten between the
// two instructions and the shift width matches the wave size; otherwise nothing.
constexpr uint32_t NggVertexCountSgpr = 3u;

[[nodiscard]] std::optional<uint32_t> FindLiveVertexExecPc(const Decoder::Program& decoded,
                                                           const CFG::Graph&       cfg,
                                                           uint32_t                wave_size);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
