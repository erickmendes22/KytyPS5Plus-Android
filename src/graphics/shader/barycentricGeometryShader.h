#ifndef EMULATOR_SRC_GRAPHICS_SHADER_BARYCENTRICGEOMETRYSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_BARYCENTRICGEOMETRYSHADER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// Primitive class the rasterizer receives, which is the input of the geometry stage.
enum class BarycentricPrimitive : uint8_t { Points, Lines, Triangles };

// A pass-through geometry stage that emulates VK_KHR_fragment_shader_barycentric for one pixel
// shader (see ShaderRecompiler::IR::BarycentricEmulation). It forwards every vertex unchanged
// and in its original order, so winding, the provoking vertex and the barycentric vertex order
// of the input primitive are kept, and adds the per-vertex copies and barycentric vectors.
struct BarycentricGeometryStage {
	BarycentricPrimitive                              primitive = BarycentricPrimitive::Triangles;
	const ShaderRecompiler::IR::BarycentricEmulation* fragment  = nullptr;
	// Built-ins the previous stage writes.
	ShaderRecompiler::IR::PreRasterOutputs previous;
	// Parameter locations the previous stage writes; varyings read from any other location are 0.
	uint32_t previous_locations = 0;
	// The device can write gl_PointSize from a geometry stage
	// (shaderTessellationAndGeometryPointSize).
	bool point_size = false;
};

[[nodiscard]] std::vector<uint32_t> BuildBarycentricGeometryShader(
    const BarycentricGeometryStage& stage);

// Varying components the generated stage writes per vertex, for the device's geometry limits.
[[nodiscard]] uint32_t BarycentricGeometryOutputComponents(const BarycentricGeometryStage& stage);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_BARYCENTRICGEOMETRYSHADER_H_
