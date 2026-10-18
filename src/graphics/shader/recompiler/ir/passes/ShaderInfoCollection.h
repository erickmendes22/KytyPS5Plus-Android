#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Collects the shader interface, resource usage and instruction features after resource tracking.
// When barycentric_available is false (no VK_KHR_fragment_shader_barycentric), pixel parameters
// are native interpolated inputs and any use that needs per-vertex data is rejected.
void CollectShaderInfo(Program& program, ShaderStageInputInfo input_info,
                       bool barycentric_available = true);

// Without barycentrics there is no raw I/J value. The I/J VGPRs are still the "previous value" of
// any exec-masked write that reuses those registers (Select operand 2 of an ExecMerge select,
// possibly through phis); that value only survives in the lanes the write skipped. Such reads are
// replaced with zero so the pair stops being a live raw read. Returns the number of replaced reads.
// Any other read of a raw I/J VGPR is left for CollectShaderInfo to reject.
// drop_carried: also the kept value of a DPP update (KYTY_DROP_CARRIED).
uint32_t DropCarriedBarycentrics(Program& program, bool drop_carried = false);

// True when a pixel shader cannot run on native interpolated inputs alone: it reads raw I/J
// values (other than ones DropCarriedBarycentrics would drop), reads P0/P10/P20 of a non-flat
// parameter, or uses flat and smooth aliases of one parameter slot. These are exactly the uses
// CollectShaderInfo rejects without barycentrics, so a shader for which this is false compiles
// on the native path.
bool PixelShaderNeedsBarycentrics(const Program& program, const ShaderPixelInputInfo& pixel,
                                  bool drop_carried = false);

// True when one parameter slot has a flat and a smooth alias, either of them in `used`.
bool PixelParameterSlotsMixFlatAndSmooth(const ShaderPixelInputInfo& pixel,
                                         const std::array<bool, 32>& used);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_ */
