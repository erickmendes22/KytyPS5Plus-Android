#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINEITEMS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINEITEMS_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/shader.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

// KYTY_GPU_PIPELINE work items (gpuPipeline.h): what the front computes for one draw or dispatch
// and the record thread consumes.
namespace Libs::Graphics::GpuPipeline {

// The register blocks of one queued item (CommandBuffer's accessors return them while the item's
// job runs, RegisterSnapshotScope). Blocks are shared between consecutive items until the CP
// changes them (RegisterSnapshotCache): about 670 items/frame, but far fewer register changes.
// The record path only reads them.
struct RegisterSnapshot {
	std::shared_ptr<const HW::Context>    context;
	std::shared_ptr<const HW::UserConfig> user_config;
	std::shared_ptr<const HW::Shader>     shaders;
};
static_assert(std::is_trivially_copyable_v<HW::Context> &&
              std::is_trivially_copyable_v<HW::UserConfig> &&
              std::is_trivially_copyable_v<HW::Shader>);

// The front's last snapshot blocks: a new item reuses a block whose bytes equal the live block
// (copy on change instead of a copy per item). Front only.
class RegisterSnapshotCache {
public:
	RegisterSnapshot Take(const CommandBuffer& buffer) {
		return {Reuse(m_last.context, buffer.LiveRegisters()),
		        Reuse(m_last.user_config, buffer.LiveUserConfig()),
		        Reuse(m_last.shaders, buffer.LiveShaders())};
	}
	[[nodiscard]] uint64_t Copies() const { return m_copies; }

private:
	template <typename Block>
	std::shared_ptr<const Block> Reuse(std::shared_ptr<const Block>& last, const Block& live) {
		if (last == nullptr || std::memcmp(last.get(), &live, sizeof(Block)) != 0) {
			last = std::make_shared<const Block>(live);
			m_copies++;
		}
		return last;
	}
	struct {
		std::shared_ptr<const HW::Context>    context;
		std::shared_ptr<const HW::UserConfig> user_config;
		std::shared_ptr<const HW::Shader>     shaders;
	} m_last;
	uint64_t m_copies = 0;
};

class RegisterSnapshotScope {
public:
	explicit RegisterSnapshotScope(const RegisterSnapshot& snapshot)
	    : m_scope(t_register_override, {const_cast<HW::Context*>(snapshot.context.get()),
	                                    const_cast<HW::UserConfig*>(snapshot.user_config.get()),
	                                    const_cast<HW::Shader*>(snapshot.shaders.get())}) {}

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
	explicit DrawItem(RegisterSnapshot snapshot): registers(std::move(snapshot)) {}
	RegisterSnapshot    registers;
	PreparedDrawShaders shaders;
	WalkFan::PrewalkList walks; // KYTY_GPU_FAN: the front's walk jobs, taken by the recorder
	bool                prepared = false;
};

// A queued dispatch.
struct DispatchItem {
	explicit DispatchItem(RegisterSnapshot snapshot): registers(std::move(snapshot)) {}
	RegisterSnapshot       registers;
	PreparedComputeProgram compute;
	WalkFan::PrewalkList   walks;
	bool                   prepared = false;
};

// Set on the record thread while a job whose item the front prepared runs; RefreshShaders and
// the dispatch path take their result from here instead of walking the SRT.
inline thread_local PreparedDrawShaders*    t_prepared_draw    = nullptr;
inline thread_local PreparedComputeProgram* t_prepared_compute = nullptr;

} // namespace Libs::Graphics::GpuPipeline

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINEITEMS_H_
