#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct NativeFloat16Stats {
	uint32_t scalar = 0; // V_ADD/SUB/SUBREV/MUL_F16 results
	uint32_t packed = 0; // V_PK_ADD/MUL_F16 lanes
};

// ShaderDeviceFeatures::native_float16: half-float add, subtract and multiply that the translator
// computes in F32 from F16 inputs and rounds back to F16 become native F16 operations:
//   ConvertF16F32(FPAdd32|FPSub32|FPMul32(ConvertF32F16(a), ConvertF32F16(b))) -> FPAdd16(a, b)
// and the same per lane of PackHalf2x16. Both round once to F16 (round to nearest even), so the
// results are identical for every non-NaN input as long as F16 denormals are preserved. FMA,
// min/max, output modifiers (omod/clamp) and transcendental ops keep the F32 path.
NativeFloat16Stats LowerNativeFloat16(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
