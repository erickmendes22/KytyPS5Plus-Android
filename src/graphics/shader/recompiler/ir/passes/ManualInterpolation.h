#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MANUALINTERPOLATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MANUALINTERPOLATION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Pixel shaders that interpolate packed parameters by hand read each vertex's raw dword
// (v_interp_mov P0/P10/P20), unpack it (16-bit unorm/snorm/half fields, or the float itself) and
// combine the three vertices with the raw I/J barycentrics: p0 + i*(p1 - p0) + j*(p2 - p0).
// Without VK_KHR_fragment_shader_barycentric those reads need a pass-through geometry stage.
//
// This pass proves, by tracking every value derived from the vertex reads as
// sum_k c_k(i, j) * x(v_k) (x = one unpacked field, c_k affine in i and j, times common factors
// that do not depend on vertex data), that each such value is only consumed where it equals
// M * ((1 - i - j) * x(v0) + i * x(v1) + j * x(v2)), i.e. M times the hardware interpolation of x.
// It then replaces every unpacked vertex value by an ordinary interpolated input that the
// previous stage computes from its own export (PackedVarying), and the I/J reads by zero, so the
// pixel shader runs without per-vertex data. Results equal the original up to float rounding
// (16-bit snorm -32768 reads as -32767, both -1.0 after the shader's scale and clamp).
//
// Unsafe uses (non-linear use of a vertex value or of a partial sum, vertex values used
// individually, flat/provoking-vertex semantics, I/J used for anything else) leave the shader
// unchanged. Exec-merge selects are taken to carry the newly written value only: the skipped lanes
// of a per-vertex register are not expected to be read again.
//
// New input locations are allocated downward from location_limit - 1, skipping the shader's own
// parameter locations. drop_carried (ShaderDeviceFeatures::drop_carried_vertex_data): a DPP
// update's kept I/J counts as carried, and kept operands that carry vertex data only in skipped
// lanes are replaced by zero, so the reads they would keep alive go away. Returns true when the shader was rewritten and
// program.info.packed_varyings lists the new inputs; reason says why not otherwise.
bool LowerManualInterpolation(Program& program, const ShaderPixelInputInfo& pixel,
                              uint32_t location_limit, bool drop_carried, std::string* reason);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MANUALINTERPOLATION_H_ */
