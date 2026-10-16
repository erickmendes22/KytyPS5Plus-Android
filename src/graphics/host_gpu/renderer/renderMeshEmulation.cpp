#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/emulatorConfig.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/MeshEmulationLayout.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <limits>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::PushData;

struct MeshEmulationSizes {
	uint64_t vertex_bytes = 0;
	uint64_t index_bytes  = 0;
	uint32_t index_count  = 0;
};

MeshEmulationSizes MeshEmulationOutputSizes(const ShaderVertexInputInfo& mesh_input,
                                            uint64_t                     slots) {
	const auto& mesh   = mesh_input.mesh;
	const auto  layout = ShaderRecompiler::IR::BuildMeshEmulationLayout(
        std::span<const ShaderRecompiler::IR::StageOutput>(mesh_input.stage.program->info.outputs));
	const uint64_t indices =
	    slots * mesh.max_primitives * ShaderRecompiler::IR::MeshEmulationIndexDwords;
	if (indices > std::numeric_limits<uint32_t>::max()) {
		EXIT("mesh emulation: draw of %" PRIu64 " workgroups needs %" PRIu64
		     " indices, more than one indexed draw takes\n",
		     slots, indices);
	}
	return {.vertex_bytes = slots * mesh.max_vertices * layout.stride_dwords * sizeof(uint32_t),
	        .index_bytes  = indices * sizeof(uint32_t),
	        .index_count  = static_cast<uint32_t>(indices)};
}

} // namespace

RenderExecutor::EmulatedMeshDraw RenderExecutor::DispatchEmulatedMesh(
    CommandBuffer& buffer, const ShaderVertexInputInfo& mesh_input,
    const ShaderProgram& mesh_program, PreparedBindings& mesh_bindings,
    std::span<const uint32_t> draw_data, uint32_t groups, uint32_t instances) {
	EXIT_IF(draw_data.size() != PushData::MeshDrawDwordCount || groups == 0 || instances == 0);
	auto& pipeline =
	    m_context.GetPipelineCache().GetMeshEmulationPipeline(mesh_input, mesh_program);
	const auto sizes = MeshEmulationOutputSizes(mesh_input, uint64_t {groups} * instances);
	const auto index_offset =
	    Common::AlignUp(sizes.vertex_bytes, MeshEmulationRing::Alignment);
	auto&      ring = m_context.GetBufferCache().GetMeshEmulationRing();
	const auto output = ring.Reserve(index_offset + sizes.index_bytes);

	// The dispatch is recorded on the primary buffer, ahead of the deferred render pass, so
	// consecutive emulated mesh draws keep sharing one pass (see CommandBuffer::BeginRendering).
	if (output.reuses_current || Config::GetRenderPassMode() < 2) {
		// Earlier draws of this command buffer still read the range being rewritten; draws of
		// the open pass would otherwise execute after this dispatch.
		m_context.GetCommandScheduler().EndRendering(PassEnd::Compute);
	}
	const auto vk_buffer = buffer.Handle();
	if (output.reuses_current) {
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eVertexInput |
		                              vk::PipelineStageFlagBits::eVertexShader,
		                          vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 0,
		                          nullptr, 0, nullptr);
	}
	PreparedBindings* stage = &mesh_bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline, std::span {&stage, 1u});

	std::array<uint32_t, PushData::MeshEmulationDrawDwordCount> push {};
	std::ranges::copy(draw_data, push.begin());
	const auto vertex_address = output.address;
	const auto index_address  = output.address + index_offset;
	push[PushData::MeshEmulationVertexAddressDword]      = static_cast<uint32_t>(vertex_address);
	push[PushData::MeshEmulationVertexAddressDword + 1u] =
	    static_cast<uint32_t>(vertex_address >> 32u);
	push[PushData::MeshEmulationIndexAddressDword]      = static_cast<uint32_t>(index_address);
	push[PushData::MeshEmulationIndexAddressDword + 1u] =
	    static_cast<uint32_t>(index_address >> 32u);
	push[PushData::MeshEmulationGroupCountDword] = groups;
	vk_buffer.pushConstants(pipeline.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                        sizeof(push), push.data());
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	buffer.MarkGpu("misc");
	vk_buffer.dispatch(groups, instances, 1);
	buffer.MarkGpu("mesh-cs", mesh_input.stage.program->shader_hash);
	Common::PerfStats::Count(Common::PerfStats::Counter::MeshDispatches);
	Common::PerfStats::Count(Common::PerfStats::Counter::MeshIndices, sizes.index_count);

	vk::BufferMemoryBarrier written {};
	written.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	written.dstAccessMask       = vk::AccessFlagBits::eIndexRead | vk::AccessFlagBits::eShaderRead;
	written.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	written.buffer              = output.buffer;
	written.offset              = output.offset;
	written.size                = index_offset + sizes.index_bytes;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                          vk::PipelineStageFlagBits::eVertexInput |
	                              vk::PipelineStageFlagBits::eVertexShader,
	                          {}, 0, nullptr, 1, &written, 0, nullptr);
	static std::atomic_uint32_t s_logged = 0;
	if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
		LOGF("Mesh emulation: MS=0x%016" PRIx64 " groups=%ux%u vertices=%" PRIu64
		     " B indices=%u\n",
		     mesh_input.stage.program->shader_hash, groups, instances, sizes.vertex_bytes,
		     sizes.index_count);
	}
	return {.vertex_address = vertex_address,
	        .index_buffer   = output.buffer,
	        .index_offset   = output.offset + index_offset,
	        .index_count    = sizes.index_count};
}

} // namespace Libs::Graphics
