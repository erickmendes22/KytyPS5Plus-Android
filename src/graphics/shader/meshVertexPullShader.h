#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MESHVERTEXPULLSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MESHVERTEXPULLSHADER_H_

#include "graphics/shader/recompiler/ir/MeshEmulationLayout.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// The vertex stage of an emulated mesh draw (see ShaderRecompiler::IR::MeshEmulationLayout). It
// reads vertex record gl_VertexIndex from the buffer whose device address is in push-data
// dwords 0..1 and writes the record's fields as the mesh stage would have: gl_Position,
// parameters at their locations as vec4, clip/cull distances and gl_Layer.
struct MeshVertexPullStage {
	const ShaderRecompiler::IR::MeshEmulationLayout* layout = nullptr;
	// With a layer field and an emulated-barycentric geometry stage after it, a flat uvec2
	// (layer, viewport index 0) is also written here, as vertex shaders do for that stage.
	uint32_t layer_viewport_location = UINT32_MAX;
};

[[nodiscard]] std::vector<uint32_t> BuildMeshVertexPullShader(const MeshVertexPullStage& stage);

// Built-ins the generated stage writes, for a following geometry stage.
[[nodiscard]] ShaderRecompiler::IR::PreRasterOutputs
MeshVertexPullOutputs(const MeshVertexPullStage& stage);

// Parameter locations the generated stage writes.
[[nodiscard]] uint32_t MeshVertexPullParameterMask(const MeshVertexPullStage& stage);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_MESHVERTEXPULLSHADER_H_
