#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINEITEMS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINEITEMS_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/shader.h"

#include <array>
#include <memory>
#include <vector>

// KYTY_GPU_PIPELINE work items (gpuPipeline.h): what the front computes for one draw or dispatch
// and the record thread consumes.
namespace Libs::Graphics::GpuPipeline {

// The register blocks of one queued item (CommandBuffer's accessors return them while the item's
// job runs, RegisterSnapshotScope).
struct RegisterSnapshot {
	HW::Context    context;
	HW::UserConfig user_config;
	HW::Shader     shaders;

	explicit RegisterSnapshot(const CommandBuffer& buffer)
	    : context(buffer.LiveRegisters()), user_config(buffer.LiveUserConfig()),
	      shaders(buffer.LiveShaders()) {}
};
static_assert(std::is_trivially_copyable_v<HW::Context> &&
              std::is_trivially_copyable_v<HW::UserConfig> &&
              std::is_trivially_copyable_v<HW::Shader>);

class RegisterSnapshotScope {
public:
	explicit RegisterSnapshotScope(RegisterSnapshot& snapshot)
	    : m_scope(t_register_override,
	              {&snapshot.context, &snapshot.user_config, &snapshot.shaders}) {}

private:
	BasicOverrideScope<RegisterOverride> m_scope;
};

// Materialized resources copied for the item: the program cache keeps one snapshot per source
// and the front's next materialization of the same source overwrites it (C2, round B4).
class ResourceCopies {
public:
	// Repoints `stage` at a private copy of its resources.
	void Own(ShaderStageRuntime& stage) {
		if (stage.resources == nullptr) {
			return;
		}
		m_copies.push_back(std::make_unique<ShaderRecompiler::IR::ResourceSnapshot>(*stage.resources));
		stage.resources = m_copies.back().get();
	}

private:
	std::vector<std::unique_ptr<ShaderRecompiler::IR::ResourceSnapshot>> m_copies;
};

// RefreshShaders' output for a draw, computed on the front.
struct PreparedDrawShaders {
	uint32_t                             color_output_mask = 0;
	bool                                 ps_active         = false;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo                 ps_input_info;
	PipelineCache::GraphicsPrograms      programs;
	ShaderIndirectDrawInfo               indirect;
	ResourceCopies                       copies;

	void OwnResources() {
		for (uint32_t i = 0; i < programs.VertexStageCount(); i++) copies.Own(vertex_info[i].stage);
		if (ps_active) copies.Own(ps_input_info.stage);
	}
};

// GetComputeProgram's output for a dispatch, computed on the front.
struct PreparedComputeProgram {
	ShaderProgram          program;
	ShaderComputeInputInfo input_info;
	ResourceCopies         copies;
};

// A queued draw: its registers and, when it walks, the front's walk.
struct DrawItem {
	explicit DrawItem(const CommandBuffer& buffer): registers(buffer) {}
	RegisterSnapshot    registers;
	PreparedDrawShaders shaders;
	bool                prepared = false;
};

// A queued dispatch.
struct DispatchItem {
	explicit DispatchItem(const CommandBuffer& buffer): registers(buffer) {}
	RegisterSnapshot       registers;
	PreparedComputeProgram compute;
	bool                   prepared = false;
};

// Set on the record thread while a job whose item the front prepared runs; RefreshShaders and
// the dispatch path take their result from here instead of walking the SRT.
inline thread_local PreparedDrawShaders*    t_prepared_draw    = nullptr;
inline thread_local PreparedComputeProgram* t_prepared_compute = nullptr;

} // namespace Libs::Graphics::GpuPipeline

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINEITEMS_H_
