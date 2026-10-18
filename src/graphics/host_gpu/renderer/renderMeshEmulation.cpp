#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/emulatorConfig.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanDebugLabel.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderIndirect.h"
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

// What differs between a direct and an indirect emulated mesh dispatch.
struct MeshDispatchSource {
	std::span<const uint32_t>             draw_data; // direct: the draw prefix
	uint32_t                              groups    = 0;
	uint32_t                              instances = 0;
	const IndirectDrawFixup::MeshRecords* indirect  = nullptr;
};

RenderExecutor::EmulatedMeshDraw RenderExecutor::DispatchEmulatedMesh(
    CommandBuffer& buffer, const ShaderVertexInputInfo& mesh_input,
    const ShaderProgram& mesh_program, PreparedBindings& mesh_bindings,
    std::span<const uint32_t> draw_data, uint32_t groups, uint32_t instances) {
	EXIT_IF(draw_data.size() != PushData::MeshDrawDwordCount || groups == 0 || instances == 0);
	return RecordEmulatedMesh(buffer, mesh_input, mesh_program, mesh_bindings,
	                          {.draw_data = draw_data, .groups = groups, .instances = instances});
}

RenderExecutor::EmulatedMeshDraw RenderExecutor::DispatchEmulatedMeshIndirect(
    CommandBuffer& buffer, const ShaderVertexInputInfo& mesh_input,
    const ShaderProgram& mesh_program, PreparedBindings& mesh_bindings,
    const IndirectDrawArgs& args, const IndirectDrawSources& sources) {
	const auto& mesh = mesh_input.mesh;
	EXIT_IF(mesh.primitives_per_group == 0 || mesh.fast_launch);
	// The most workgroups one instance can take: every index of INDEX_BUFFER_SIZE drawn.
	const auto primitives = mesh.InputPrimitiveCount(args.index_buffer_size);
	const auto groups     = primitives == 0 ? 1u : (primitives - 1u) / mesh.primitives_per_group + 1u;
	// Outputs of one workgroup slot, and at most a quarter of the mesh ring for one draw.
	const auto     slot_bytes = MeshEmulationOutputSizes(mesh_input, 1);
	const uint64_t per_slot   = slot_bytes.vertex_bytes + slot_bytes.index_bytes;
	const uint64_t max_slots  = std::max<uint64_t>(
        1u, MeshEmulationRing::InitialSize / 4u / std::max<uint64_t>(per_slot, 1u));
	const IndirectDrawFixup::MeshShape shape {
	    .primitive_size       = mesh.InputPrimitiveSize(),
	    .primitive_step       = mesh.InputPrimitiveStep(),
	    .primitives_per_group = mesh.primitives_per_group,
	    .indices_per_group    = slot_bytes.index_count,
	    .slots = m_indirect_fixup->MeshSlots(groups, static_cast<uint32_t>(
	                                                     std::min<uint64_t>(max_slots, UINT32_MAX))),
	};
	const auto records = m_indirect_fixup->RecordMesh(buffer, args, sources, shape);
	auto       draw    = RecordEmulatedMesh(buffer, mesh_input, mesh_program, mesh_bindings,
	                                        {.groups = shape.slots, .instances = 1, .indirect = &records});
	draw.draw_buffer = records.buffer;
	draw.draw_offset = records.draw_offset;
	return draw;
}

RenderExecutor::EmulatedMeshDraw RenderExecutor::RecordEmulatedMesh(
    CommandBuffer& buffer, const ShaderVertexInputInfo& mesh_input,
    const ShaderProgram& mesh_program, PreparedBindings& mesh_bindings,
    const MeshDispatchSource& source) {
	const auto groups    = source.groups;
	const auto instances = source.instances;
	auto& pipeline =
	    m_context.GetPipelineCache().GetMeshEmulationPipeline(mesh_input, mesh_program);
	const auto sizes = MeshEmulationOutputSizes(mesh_input, uint64_t {groups} * instances);
	const auto index_offset =
	    Common::AlignUp(sizes.vertex_bytes, MeshEmulationRing::Alignment);
	auto&      ring = m_context.GetBufferCache().GetMeshEmulationRing();
	const auto output = ring.Reserve(index_offset + sizes.index_bytes);

	// The dispatch is recorded on the primary buffer, ahead of the deferred render pass, so
	// consecutive emulated mesh draws keep sharing one pass (see CommandBuffer::BeginRendering).
	if (output.reuses_current || CommandBuffer::RenderPassMode(m_context.GetGraphics()) < 2) {
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
	if (source.indirect != nullptr) {
		// PushData::MeshIndirect*: the fix-up pass's record replaces the draw prefix.
		push[0] = static_cast<uint32_t>(source.indirect->record);
		push[1] = static_cast<uint32_t>(source.indirect->record >> 32u);
	} else {
		std::ranges::copy(source.draw_data, push.begin());
	}
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
	// KYTY_LIVE_KNOBS mesh-skip-cs=: the draw reads what the range already holds.
	const bool skip_dispatch = LiveKnobs::SkipMeshDispatch(mesh_input.stage.program->shader_hash);
	if (!skip_dispatch) {
		const ScopedDebugLabel label(vk_buffer, "mesh-cs", mesh_input.stage.program->shader_hash);
		if (source.indirect != nullptr) {
			vk_buffer.dispatchIndirect(source.indirect->buffer, source.indirect->dispatch_offset);
		} else {
			vk_buffer.dispatch(groups, instances, 1);
		}
	}
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
		LOGF("Mesh emulation: MS=0x%016" PRIx64 " groups=%ux%u%s vertices=%" PRIu64
		     " B indices=%u\n",
		     mesh_input.stage.program->shader_hash, groups, instances,
		     source.indirect != nullptr ? " (indirect, reserved)" : "", sizes.vertex_bytes,
		     sizes.index_count);
	}
	const uint64_t vertex_count = uint64_t {groups} * instances * mesh_input.mesh.max_vertices;
	EXIT_IF(vertex_count > UINT32_MAX);
	ring.PollOutOfRangeCounter();
	return {.vertex_address       = vertex_address,
	        .index_buffer         = output.buffer,
	        .index_offset         = output.offset + index_offset,
	        .index_count          = sizes.index_count,
	        .vertex_count         = static_cast<uint32_t>(vertex_count),
	        .out_of_range_counter = ring.OutOfRangeCounterAddress()};
}

} // namespace Libs::Graphics
