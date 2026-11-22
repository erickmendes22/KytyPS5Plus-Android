#include "graphics/host_gpu/renderer/renderIndirect.h"
#include "graphics/host_gpu/renderer/demonsRollingProbe.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/indirect_draw_fixup_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/guest_gpu/graphicsRun.h" // GetGpu().GetFrameNum() needs complete GuestGpu
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/pipeline/bindMemo.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <cstdlib>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

constexpr uint32_t FixupGroupSize     = 64;
constexpr uint32_t IndexedRecordBytes = 5 * sizeof(uint32_t);
constexpr uint32_t DirectRecordBytes  = 4 * sizeof(uint32_t);
constexpr size_t FeedbackBytes = 4 * sizeof(uint32_t); // clamp flag, original X/Y/Z
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
	    FeedbackSlots * FeedbackBytes);
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
	// The same GPU image/buffer alias issue affects GPU-culled geometry draws.
	// A stale argument or count buffer can remove the entire opaque scene,
	// while separately generated HUD/particle paths remain visible.
	// Only synchronize genuine GPU-image-owned ranges; do not synthesize
	// nonzero draw counts or read the arguments synchronously on the CPU.
	static const bool ds_draw_args_gpu_owned = [] {
		const char* flag = std::getenv("KYTY_DS_GPU_DRAW_ARGS");
		return flag != nullptr && flag[0] == '1' && flag[1] == '\0';
	}();
	if (ds_draw_args_gpu_owned) {
		auto& images = m_context.GetTextureCache();
		const auto publish = [&](uint64_t address, uint64_t bytes) {
			if (address != 0 && bytes <= (uint64_t {256} << 20u) &&
			    images.IsRegionGpuModified(address, bytes)) {
				m_context.GetCommandScheduler().EndRendering(PassEnd::Compute);
				(void)images.PublishGpuImageForIndirectArgs(address, bytes);
			}
		};
		publish(args.args_addr, size);
		if (args.count_addr != 0) publish(args.count_addr, sizeof(uint32_t));
	}
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
	// PPSA01341 may generate GPU indirect dispatch records through a
	// color/depth attachment aliased into the guest argument buffer. Shader
	// and transfer writes alone do not make such an attachment producer
	// available to this shader-buffer-address fix-up. This is strictly the
	// Demon's Souls indirect-dispatch path; don't globally serialize draws.
	static const bool ds_gpu_indirect = [] {
		const char* value = std::getenv("KYTY_DS_GPU_ZERO_INDIRECT");
		return value && value[0] == '1' && value[1] == '\0';
	}();
	if (ds_gpu_indirect && fixup.mode == ModeDispatchClamp) {
		before.srcAccessMask |= vk::AccessFlagBits::eColorAttachmentWrite |
		                        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
	}
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

vk::DeviceAddress IndirectDrawFixup::AcquireFeedback(uint32_t& slot, bool lighting) {
    // The first 48 words record indirect draws and the final 16 words
    // measure the six Demon's Souls indirect lighting passes. Otherwise
    // thousands of draws can starve the lighting diagnostic completely.
    // No guest command or GPU synchronization changes.
    static const bool ds_trace = [] {
        const char* v = std::getenv("KYTY_DS_FRAME_TRACE");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    const uint32_t first = ds_trace && lighting ? 48u : 0u;
    const uint32_t end   = ds_trace ? (lighting ? FeedbackSlots : 48u) : FeedbackSlots;
    uint32_t& next = ds_trace && lighting ? m_lighting_feedback_next : m_feedback_next;
    if (next < first || next >= end) next = first;
    // Reuse any free slot, not just the cursor. Completed GPU work is
    // released by PollFeedback; never stall waiting for an unfinished slot.
    for (uint32_t attempt = 0; attempt < end - first; ++attempt) {
        const uint32_t index = next;
        next = index + 1u == end ? first : index + 1u;
        if (m_feedback[index].tick != 0) continue;
        slot = index;
        const uint32_t zero = 0;
        const size_t offset = slot * FeedbackBytes;
        std::memcpy(m_feedback_buffer->Mapped().data() + offset, &zero, sizeof(zero));
        m_feedback_buffer->Flush(offset, sizeof(zero));
        return m_feedback_buffer->BufferDeviceAddress() + offset;
    }
    if (ds_trace && lighting) {
        static std::atomic<uint32_t> missed {0};
        const auto sample = missed.fetch_add(1u, std::memory_order_relaxed);
        if (sample < 8u || (sample % 4096u) == 0u)
            Log::WriteToConsoleAndLog(fmt::sprintf(
                "[DS_LIGHTING_FEEDBACK] all 16 reserved GPU slots busy, missed=%u\n", sample));
    }
    return 0;
}

void IndirectDrawFixup::CommitFeedback(uint32_t slot, uint64_t site, bool mesh, uint32_t reserved,
                                       bool dispatch, uint64_t shader_hash) {
	m_feedback[slot] = {.tick     = m_context.GetCommandScheduler().CurrentTick(),
	                    .site     = site,
	                    .shader_hash = shader_hash,
	                    .frame = m_context.GetGpu().GetFrameNum(), // stamp indirect DRAW and dispatch for real frame attribution
	                    .reserved = reserved,
	                    .mesh     = mesh,
	                    .dispatch = dispatch};
}

IndirectDrawFixup::DispatchRecord IndirectDrawFixup::RecordDispatchClamp(
    CommandBuffer& buffer, vk::DeviceAddress args, const std::array<uint32_t, 3>& limits,
    uint64_t site, uint64_t shader_hash) {
	EXIT_IF(args == 0);
	PollFeedback();
	uint32_t    feedback_slot = 0;
	FixupParams params {};
	params.source        = args;
	
	params.max_count     = 1;
	params.mode          = ModeDispatchClamp;
	params.max_groups    = limits[0];
	params.max_instances = limits[1];
	params.max_z         = limits[2];
	// Only the six known PPSA01341 lighting kernels, during explicit frame
	// tracing, request extra information. All other games retain the exact
	// old GPU command and one-bit clamp feedback semantics.
	static const bool ds_trace = [] {
		const char* v = std::getenv("KYTY_DS_FRAME_TRACE");
		return v != nullptr && v[0] == '1' && v[1] == '\0';
	}();
	constexpr std::array<uint64_t, 6> lighting_hashes {{
	    0xc2e01fbe98f5ebabull, 0x23056085c1120461ull,
	    0x3dfe402653925fbfull, 0xfabe0bcb275a3005ull,
	    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
	}};
	const bool lighting = ds_trace &&
	    std::find(lighting_hashes.begin(), lighting_hashes.end(), shader_hash) !=
	        lighting_hashes.end();
	params.feedback = AcquireFeedback(feedback_slot, lighting);
	params.padding = lighting ? 1u : 0u;
	const auto output = Dispatch(buffer, sizeof(vk::DispatchIndirectCommand), &params, 1);
	if (params.feedback != 0) {
		CommitFeedback(feedback_slot, site, false, lighting ? 1u : 0u, true,
		               lighting ? shader_hash : 0ull);
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
		const size_t offset = slot * FeedbackBytes;
		const size_t bytes = feedback.dispatch && feedback.reserved == 1u
		                         ? FeedbackBytes : sizeof(uint32_t);
		std::array<uint32_t, 4> words {};
		m_feedback_buffer->Invalidate(offset, bytes);
		std::memcpy(words.data(), m_feedback_buffer->Mapped().data() + offset, bytes);
		const uint32_t requested = words[0];
		if (feedback.dispatch) {
			static std::atomic<uint32_t> clamp_count {0};
			const bool clamped = requested != 0;
			if (feedback.reserved == 1u) {
				const uint32_t x = words[1], y = words[2], z = words[3];
				// Capture the actual completed GPU argument values throughout
				// the session, not only before arbitrary frame deadlines.
				constexpr std::array<uint64_t, 6> watched {{
				    0xc2e01fbe98f5ebabull, 0x23056085c1120461ull,
				    0x3dfe402653925fbfull, 0xfabe0bcb275a3005ull,
				    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
				}};
				const auto found = std::find(watched.begin(), watched.end(), feedback.shader_hash);
				static std::array<DemonsRollingProbe::FrameGate, 6> lighting_samples {};
				if (found != watched.end() &&
				    lighting_samples[static_cast<size_t>(found - watched.begin())].Due(
				        feedback.frame, 32)) {
					Log::WriteToConsoleAndLog(fmt::sprintf(
					    "[DS_LIGHTING_GROUPS] source=gpu frame=%d shader=0x%016" PRIx64
					    " args=0x%016" PRIx64 " x=%u y=%u z=%u saturated=0 "
					    "clamped=%u zero=%u\n",
					    feedback.frame, feedback.shader_hash, feedback.site, x, y, z,
					    clamped ? 1u : 0u,
					    (x == 0u || y == 0u || z == 0u) ? 1u : 0u));
				}
			}
			if (clamped && clamp_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("GPU indirect dispatch: guest record 0x%016" PRIx64 " asked for more "
				     "workgroups than maxComputeWorkGroupCount; clamped (work dropped)\n",
				     feedback.site);
			}
			feedback = {};
			continue;
		}
		NoteCpuResult(feedback.site, requested == 0);
		// vkCmdDrawIndexedIndirect can be accepted with indexCount=0.
		// The DS graphics accepted-draw counter cannot distinguish this.
		// Count already-completed GPU fixup feedback, never wait on a fence.
		static const bool ds_trace = [] {
			const char* v = std::getenv("KYTY_DS_FRAME_TRACE");
			return v != nullptr && v[0] == '1' && v[1] == '\0';
		}();
		if (ds_trace) {
			// GPU-culled draw zeros may be expected, or may explain the
			// missing deferred geometry. Aggregate actual completed GPU
			// records per source-frame window, without a CPU readback stall
			// or replacing any draw count. The first and 128th/256th etc
			// records provide bounded, comparable bright/onset/dark evidence.
			if (feedback.frame >= 0) {
				struct Period {
					int32_t bucket = -1;
					uint32_t total = 0;
					uint32_t nonempty = 0;
				};
				static std::array<Period, 64> periods {};
				const int32_t bucket = feedback.frame / 24;
				auto& period = periods[static_cast<uint32_t>(bucket) % periods.size()];
				if (period.bucket != bucket) {
					period = {bucket, 0u, 0u};
				}
				++period.total;
				period.nonempty += requested != 0u;
				if (period.total == 1u || period.total % 128u == 0u) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_INDIRECT_FRAME_COUNTS] submitted={}..{} completed={} "
					    "nonempty={} zero={} last_site=0x{:x}\n",
					    bucket * 24, bucket * 24 + 23, period.total,
					    period.nonempty, period.total - period.nonempty, feedback.site));
				}
			}
			static std::atomic_uint64_t feedback_total {0};
			static std::atomic_uint64_t feedback_nonempty {0};
			const uint64_t n = feedback_total.fetch_add(1u, std::memory_order_relaxed) + 1u;
			if (requested != 0u) feedback_nonempty.fetch_add(1u, std::memory_order_relaxed);
			if (n <= 12u || n % 512u == 0u) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_INDIRECT_FEEDBACK] completed={} nonempty={} empty={} "
				    "last_site=0x{:x} last_requested={} mesh={}\n",
				    n, feedback_nonempty.load(std::memory_order_relaxed),
				    n - feedback_nonempty.load(std::memory_order_relaxed),
				    feedback.site, requested, feedback.mesh ? 1 : 0));
			}
		}
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
