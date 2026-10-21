#include "graphics/host_gpu/renderer/renderIndirect.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/indirect_draw_fixup_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/pipeline/bindMemo.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {

namespace {

constexpr uint32_t FixupGroupSize     = 64;
constexpr uint32_t IndexedRecordBytes = 5 * sizeof(uint32_t);
constexpr uint32_t DirectRecordBytes  = 4 * sizeof(uint32_t);
// Mesh outputs (indirect_draw_fixup.comp MESH_*): record, dispatch, draw.
constexpr uint64_t MeshRecordBytes    = 12 * sizeof(uint32_t);
constexpr uint64_t MeshDispatchOffset = 12 * sizeof(uint32_t);
constexpr uint64_t MeshDrawOffset     = 16 * sizeof(uint32_t);
constexpr uint64_t MeshOutputBytes    = 21 * sizeof(uint32_t);
// indirect_draw_fixup.comp `mode` values.
constexpr uint32_t ModeDraws          = 0;
constexpr uint32_t ModeMesh           = 1;
constexpr uint32_t ModeDispatchClamp  = 2;

// Mirrors the push constants of indirect_draw_fixup.comp.
struct FixupParams {
	uint64_t source               = 0;
	uint64_t destination          = 0;
	uint64_t count_source         = 0;
	uint64_t index_base           = 0;
	uint64_t feedback             = 0;
	uint32_t stride_words         = 0;
	uint32_t max_count            = 0;
	uint32_t index_limit          = 0;
	uint32_t indexed              = 0;
	uint32_t mode                 = 0; // ModeDraws / ModeMesh / ModeDispatchClamp
	uint32_t element_bytes        = 0;
	uint32_t primitive_size       = 0;
	uint32_t primitive_step       = 0;
	uint32_t primitives_per_group = 0;
	uint32_t indices_per_group    = 0;
	uint32_t max_slots            = 0;
	uint32_t max_groups           = 0; // x (mesh: groups) limit
	uint32_t max_instances        = 0; // y (mesh: instances) limit
	uint32_t max_z                = 0; // ModeDispatchClamp: z limit
	uint32_t padding              = 0;
};
static_assert(sizeof(FixupParams) == 104);

uint32_t RecordBytes(bool indexed) {
	return indexed ? IndexedRecordBytes : DirectRecordBytes;
}

} // namespace

IndirectDrawFixup::IndirectDrawFixup(RenderContext& context): m_context(context) {
	auto& graphics = context.GetGraphics();
	m_multi_draw   = graphics.optional_features.multi_draw_indirect;
	const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(FixupParams)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout_info, nullptr, &m_layout),
	                     "create indirect-draw fix-up pipeline layout");

	const auto module = CompileSPV(INDIRECT_DRAW_FIXUP_SPV, graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_layout;
	const auto result =
	    graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create indirect-draw fix-up pipeline");
	SetVulkanObjectNameF(graphics.device, m_pipeline, "Indirect Draw Fix-up");

	m_feedback_buffer = std::make_unique<Buffer>(
	    graphics, context.GetCommandScheduler(), MemoryUsage::Download, 0,
	    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
	    FeedbackSlots * sizeof(uint32_t));
	LOGF("GPU indirect draws: fix-up pipeline ready (multiDrawIndirect=%u)\n",
	     m_multi_draw ? 1u : 0u);
}

IndirectDrawFixup::~IndirectDrawFixup() {
	const auto device = m_context.GetGraphics().device;
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_layout, nullptr);
}

uint64_t IndirectDrawFixup::GuestRecordsSize(const IndirectDrawArgs& args) {
	if (args.max_count == 0 || args.stride < RecordBytes(args.indexed)) {
		return 0;
	}
	const uint64_t size =
	    uint64_t {args.stride} * (args.max_count - 1u) + RecordBytes(args.indexed);
	// Every record must be readable guest memory: a GPU-written count may cover all of them.
	return LibKernel::Memory::ClampRangeSize(args.args_addr, size) == size ? size : 0;
}

IndirectDrawFixup::Sources IndirectDrawFixup::AcquireSources(const IndirectDrawArgs& args) {
	auto&      cache = m_context.GetBufferCache();
	const auto size  = GuestRecordsSize(args);
	EXIT_IF(size == 0);
	Sources sources;
	const auto [records, records_offset] = cache.ObtainBuffer(args.args_addr, size, false);
	EXIT_IF(records == nullptr);
	sources.args = records->BufferDeviceAddress() + records_offset;
	if (args.count_addr != 0) {
		const auto [count, count_offset] =
		    cache.ObtainBuffer(args.count_addr, sizeof(uint32_t), false);
		EXIT_IF(count == nullptr);
		sources.count = count->BufferDeviceAddress() + count_offset;
	}
	return sources;
}

IndirectDrawFixup::Output IndirectDrawFixup::Dispatch(CommandBuffer& buffer, uint64_t output_bytes,
                                                      void* params, uint32_t groups) {
	auto&      ring      = m_context.GetBufferCache().GetIndirectDrawRing();
	const auto output    = ring.Reserve(output_bytes);
	auto&      scheduler = m_context.GetCommandScheduler();
	if (output.reuses_current || CommandBuffer::RenderPassMode(m_context.GetGraphics()) == 0) {
		// Immediate passes record into the primary buffer; draws of this command buffer may
		// also still read a reused range.
		scheduler.EndRendering(PassEnd::Compute);
	}
	auto& fixup       = *static_cast<FixupParams*>(params);
	fixup.destination = output.address;
	const auto command = buffer.Handle();
	// Guest records were written by earlier shaders or copies of this command buffer; a reused
	// ring range was read by earlier indirect draws.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite |
	                       vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr,
	                        0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(FixupParams),
	                      params);
	buffer.MarkGpu("misc");
	command.dispatch(groups, 1, 1);
	buffer.MarkGpu("indirect-fixup");
	// Draws and dispatches read the commands; emulated mesh stages read their record.
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eDrawIndirect |
	                            vk::PipelineStageFlagBits::eComputeShader,
	                        {}, 1, &after, 0, nullptr, 0, nullptr);
	return {.buffer = output.buffer, .offset = output.offset, .address = output.address};
}

IndirectDrawFixup::Records IndirectDrawFixup::Record(CommandBuffer& buffer,
                                                     const IndirectDrawArgs& args,
                                                     const Sources& sources) {
	const auto  record_bytes = RecordBytes(args.indexed);
	PollFeedback();
	uint32_t    feedback_slot = 0;
	FixupParams params {};
	params.source       = sources.args;
	params.count_source = sources.count;
	params.feedback     = AcquireFeedback(feedback_slot);
	params.stride_words = args.stride / sizeof(uint32_t);
	params.max_count    = args.max_count;
	params.index_limit  = args.index_buffer_size;
	params.indexed      = args.indexed ? 1u : 0u;
	const auto output   = Dispatch(buffer, uint64_t {record_bytes} * args.max_count, &params,
	                               (args.max_count + FixupGroupSize - 1u) / FixupGroupSize);
	if (params.feedback != 0) {
		CommitFeedback(feedback_slot, args.args_addr, false, 0);
	}
	return {.buffer = output.buffer,
	        .offset = output.offset,
	        .stride = record_bytes,
	        .count  = args.max_count};
}

vk::DeviceAddress IndirectDrawFixup::AcquireFeedback(uint32_t& slot) {
	slot = m_feedback_next;
	if (m_feedback[slot].tick != 0) {
		return 0; // Still in flight (PollFeedback consumed every completed one).
	}
	m_feedback_next = (m_feedback_next + 1u) % FeedbackSlots;
	const uint32_t zero = 0;
	std::memcpy(m_feedback_buffer->Mapped().data() + slot * sizeof(uint32_t), &zero, sizeof(zero));
	m_feedback_buffer->Flush(slot * sizeof(uint32_t), sizeof(uint32_t));
	return m_feedback_buffer->BufferDeviceAddress() + slot * sizeof(uint32_t);
}

void IndirectDrawFixup::CommitFeedback(uint32_t slot, uint64_t site, bool mesh, uint32_t reserved,
                                       bool dispatch) {
	m_feedback[slot] = {.tick     = m_context.GetCommandScheduler().CurrentTick(),
	                    .site     = site,
	                    .reserved = reserved,
	                    .mesh     = mesh,
	                    .dispatch = dispatch};
}

IndirectDrawFixup::DispatchRecord IndirectDrawFixup::RecordDispatchClamp(
    CommandBuffer& buffer, vk::DeviceAddress args, const std::array<uint32_t, 3>& limits,
    uint64_t site) {
	EXIT_IF(args == 0);
	PollFeedback();
	uint32_t    feedback_slot = 0;
	FixupParams params {};
	params.source        = args;
	params.feedback      = AcquireFeedback(feedback_slot);
	params.max_count     = 1;
	params.mode          = ModeDispatchClamp;
	params.max_groups    = limits[0];
	params.max_instances = limits[1];
	params.max_z         = limits[2];
	const auto output    = Dispatch(buffer, sizeof(vk::DispatchIndirectCommand), &params, 1);
	if (params.feedback != 0) {
		CommitFeedback(feedback_slot, site, false, 0, true);
	}
	return {.buffer = output.buffer, .offset = output.offset};
}

bool IndirectDrawFixup::LikelyEmpty(uint64_t args_addr) {
	PollFeedback();
	const auto found = m_empty.find(args_addr);
	return found != m_empty.end() && found->second;
}

void IndirectDrawFixup::NoteCpuResult(uint64_t args_addr, bool empty) {
	// Bounded: GPU-culled draws come from a handful of record addresses.
	if (m_empty.size() >= 4096 && !m_empty.contains(args_addr)) {
		m_empty.clear();
	}
	m_empty[args_addr] = empty;
}

void IndirectDrawFixup::PollFeedback() {
	const auto& master = m_context.GetCommandScheduler().GetMasterSemaphore();
	for (uint32_t slot = 0; slot < FeedbackSlots; slot++) {
		auto& feedback = m_feedback[slot];
		if (feedback.tick == 0 || !master.IsFree(feedback.tick)) {
			continue;
		}
		m_feedback_buffer->Invalidate(slot * sizeof(uint32_t), sizeof(uint32_t));
		uint32_t requested = 0;
		std::memcpy(&requested, m_feedback_buffer->Mapped().data() + slot * sizeof(uint32_t),
		            sizeof(requested));
		if (feedback.dispatch) {
			static std::atomic<uint32_t> clamp_count {0};
			if (requested != 0 && clamp_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("GPU indirect dispatch: guest record 0x%016" PRIx64 " asked for more "
				     "workgroups than maxComputeWorkGroupCount; clamped (work dropped)\n",
				     feedback.site);
			}
			feedback = {};
			continue;
		}
		NoteCpuResult(feedback.site, requested == 0);
		if (!feedback.mesh) {
			feedback = {};
			continue;
		}
		if (requested > feedback.reserved) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("GPU indirect draw: emulated mesh draw asked for %u workgroup slots, %u were "
				     "reserved; it drew fewer instances\n",
				     requested, feedback.reserved);
			}
		}
		m_mesh_slots_seen = std::max(m_mesh_slots_seen, requested);
		feedback          = {};
	}
}

uint32_t IndirectDrawFixup::MeshSlots(uint32_t groups, uint32_t max_slots) {
	PollFeedback();
	// Astro Bot's GPU-culled particle draw: one workgroup per instance, up to 512 instances.
	constexpr uint64_t InitialInstances = 512;
	const uint64_t     wanted = std::max(uint64_t {groups} * InitialInstances,
	                                     uint64_t {m_mesh_slots_seen} * 2u);
	return static_cast<uint32_t>(std::clamp<uint64_t>(wanted, 1u, std::max(max_slots, 1u)));
}

IndirectDrawFixup::MeshRecords IndirectDrawFixup::RecordMesh(CommandBuffer& buffer,
                                                             const IndirectDrawArgs& args,
                                                             const Sources& sources,
                                                             const MeshShape& shape) {
	EXIT_IF(!args.indexed || args.max_count != 1 || args.count_addr != 0 || shape.slots == 0);
	const auto& limits = m_context.GetGraphics().GetPhysicalDeviceProperties().limits;
	PollFeedback();
	uint32_t    feedback_slot = 0;
	FixupParams params {};
	params.source = sources.args;
	params.index_base           = args.index_base_addr;
	params.feedback             = AcquireFeedback(feedback_slot);
	params.stride_words         = args.stride / sizeof(uint32_t);
	params.max_count            = 1;
	params.index_limit          = args.index_buffer_size;
	params.indexed              = 1;
	params.mode                 = ModeMesh;
	params.element_bytes        = args.index_type_and_size == 1u ? 4u : 2u;
	params.primitive_size       = shape.primitive_size;
	params.primitive_step       = shape.primitive_step;
	params.primitives_per_group = shape.primitives_per_group;
	params.indices_per_group    = shape.indices_per_group;
	params.max_slots            = shape.slots;
	params.max_groups           = limits.maxComputeWorkGroupCount[0];
	params.max_instances        = limits.maxComputeWorkGroupCount[1];
	const auto output           = Dispatch(buffer, MeshOutputBytes, &params, 1);
	if (params.feedback != 0) {
		CommitFeedback(feedback_slot, args.args_addr, true, shape.slots);
	}
	static_assert(MeshRecordBytes <= MeshDispatchOffset);
	return {.buffer          = output.buffer,
	        .record          = output.address,
	        .dispatch_offset = output.offset + MeshDispatchOffset,
	        .draw_offset     = output.offset + MeshDrawOffset};
}

void IndirectDrawFixup::Draw(vk::CommandBuffer pass, const Records& records, bool indexed) const {
	const auto draws = m_multi_draw ? 1u : records.count;
	const auto count = m_multi_draw ? records.count : 1u;
	for (uint32_t i = 0; i < draws; i++) {
		const auto offset = records.offset + uint64_t {i} * records.stride;
		if (indexed) {
			pass.drawIndexedIndirect(records.buffer, offset, count, records.stride);
		} else {
			pass.drawIndirect(records.buffer, offset, count, records.stride);
		}
	}
}

RenderExecutor::RenderExecutor(RenderContext& context): m_context(context) {
	BindMemo::SetDeviceDefault(m_context.GetGraphics().IsMesaTurnip());
}

IndirectDrawFixup& RenderExecutor::GetIndirectDrawFixup() {
	if (!m_indirect_fixup) {
		m_indirect_fixup = std::make_unique<IndirectDrawFixup>(m_context);
	}
	return *m_indirect_fixup;
}

bool RenderExecutor::IndirectDrawLikelyEmpty(uint64_t args_addr) {
	return m_context.GetGraphics().optional_features.gpu_indirect_draws &&
	       GetIndirectDrawFixup().LikelyEmpty(args_addr);
}

void RenderExecutor::NoteIndirectDrawCpuResult(uint64_t args_addr, bool empty) {
	if (m_context.GetGraphics().optional_features.gpu_indirect_draws) {
		GetIndirectDrawFixup().NoteCpuResult(args_addr, empty);
	}
}

RenderExecutor::~RenderExecutor() = default;

} // namespace Libs::Graphics
