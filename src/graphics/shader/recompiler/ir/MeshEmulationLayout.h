#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_MESHEMULATIONLAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_MESHEMULATIONLAYOUT_H_

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Mesh emulation (no VK_EXT_mesh_shader). The mesh stage runs as a compute shader with the same
// workgroup layout. Workgroup (x, y) owns record slot y * groups_x + x: max_vertices vertex
// records of MeshEmulationLayout::stride_dwords dwords, and max_primitives triangles of three
// uint32 indices into the whole vertex buffer. Unused and culled triangle slots repeat one
// index, so the rasterizer drops them; a generated vertex shader reads the records back by
// gl_VertexIndex for one indexed triangle-list draw. Primitive order follows workgroup order,
// as with native mesh shading.
inline constexpr uint32_t MeshEmulationIndexDwords = 3u;

struct MeshEmulationField {
	StageOutputKind kind       = StageOutputKind::Position;
	uint32_t        index      = 0; // Export index (clip/cull plane, parameter index).
	uint32_t        location   = 0; // Parameter location.
	uint32_t        offset     = 0; // Dword offset in the vertex record.
	uint32_t        components = 0;

	bool operator==(const MeshEmulationField& other) const = default;
};

struct MeshEmulationLayout {
	std::vector<MeshEmulationField> fields;
	uint32_t                        stride_dwords       = 0;
	uint32_t                        clip_distance_count = 0;
	uint32_t                        cull_distance_count = 0;

	[[nodiscard]] const MeshEmulationField* Find(StageOutputKind kind, uint32_t index) const {
		for (const auto& field: fields) {
			if (field.kind == kind && field.index == index) {
				return &field;
			}
		}
		return nullptr;
	}
	[[nodiscard]] bool Has(StageOutputKind kind) const {
		for (const auto& field: fields) {
			if (field.kind == kind) {
				return true;
			}
		}
		return false;
	}

	bool operator==(const MeshEmulationLayout& other) const = default;
};

// Mesh outputs are what the native mesh path supports: Position, Parameter, Clip/CullDistance
// and Layer. Layer is a per-vertex record field; the rasterizer takes it from the provoking
// vertex, which is where the native mesh path reads its per-primitive value from.
[[nodiscard]] inline uint32_t MeshEmulationComponents(StageOutputKind kind) {
	switch (kind) {
		case StageOutputKind::Position:
		case StageOutputKind::Parameter: return 4u;
		case StageOutputKind::ClipDistance:
		case StageOutputKind::CullDistance:
		case StageOutputKind::Layer: return 1u;
		default:
			EXIT("mesh emulation: unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
	return 0u;
}

[[nodiscard]] inline MeshEmulationLayout BuildMeshEmulationLayout(
    std::span<const StageOutput> outputs) {
	MeshEmulationLayout layout;
	layout.fields.reserve(outputs.size());
	for (const auto& output: outputs) {
		if (layout.Find(output.kind, output.index) != nullptr) {
			continue;
		}
		const auto components = MeshEmulationComponents(output.kind);
		layout.fields.push_back({output.kind, output.index, output.location, layout.stride_dwords,
		                         components});
		layout.stride_dwords += components;
		if (output.kind == StageOutputKind::ClipDistance) {
			layout.clip_distance_count = std::max(layout.clip_distance_count, output.index + 1u);
		} else if (output.kind == StageOutputKind::CullDistance) {
			layout.cull_distance_count = std::max(layout.cull_distance_count, output.index + 1u);
		}
	}
	// A record is never empty, so slot addressing stays meaningful for position-less shaders.
	layout.stride_dwords = std::max(layout.stride_dwords, 1u);
	return layout;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_MESHEMULATIONLAYOUT_H_
