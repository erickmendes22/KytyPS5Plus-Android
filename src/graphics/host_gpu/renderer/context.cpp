#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "common/profiler.h"
#include "graphics/host_gpu/gpuFatal.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/gpuTimer.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdlib>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()), m_scheduler(scheduler),
      m_hoist_barriers(RenderPassMode(scheduler.Graphics()) != 0 && Config::GetLayoutMode() >= 2),
      m_timer(scheduler.Timer()) {}

uint32_t CommandBuffer::RenderPassMode(const GraphicContext& graphics) {
	static const uint32_t mode = [&graphics] {
		const auto configured = Config::GetRenderPassMode();
		// Auto: immediate passes. Deferred passes (secondary command buffers) were measured
		// slower on Turnip v32 (Astro Bot intro: the hot 1080p single-draw pass went from
		// 3.3 ms to 9.2 ms each), so modes 1/2 stay opt-in.
		const auto resolved = configured == Config::RENDER_PASS_MODE_AUTO ? 0u : configured;
		LOGF("Render pass mode: %u%s (vendor 0x%04x), layout mode %u\n", resolved,
		     configured == Config::RENDER_PASS_MODE_AUTO ? " (auto)" : "",
		     graphics.GetPhysicalDeviceProperties().vendorID, Config::GetLayoutMode());
		return resolved;
	}();
	return mode;
}

void CommandBuffer::MarkGpu(const char* kind, uint64_t hash) const {
	if (m_timer.Enabled()) {
		m_timer.Mark(Handle(), kind, hash);
	}
}

void CommandBuffer::MarkGpu(const char* kind, uint64_t hash, const GpuPassInfo& info) const {
	// Inside a deferred pass the primary buffer runs before the pass's draws; no marks there.
	if (m_timer.Enabled() && m_pass_buffer == nullptr) {
		m_timer.Mark(Handle(), kind, hash, info);
	}
}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	return m_buffer;
}

vk::CommandBuffer CommandBuffer::PassHandle() const {
	if (!m_rendering) {
		EXIT("render pass commands recorded outside an open render pass\n");
	}
	return m_pass_buffer != nullptr ? m_pass_buffer : Handle();
}

bool CommandBuffer::DefersRenderPasses() const {
	static const bool deferred = RenderPassMode(m_graphics) != 0;
	return deferred;
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	CheckGpuResult("vkBeginCommandBuffer", result);
}

void CommandBuffer::End() const {
	EndRendering(PassEnd::Submit);
	auto buffer = Handle();

	auto result = buffer.end();

	CheckGpuResult("vkEndCommandBuffer", result);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	if (m_rendering && m_render_state == state) {
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering(PassEnd::State);
	MarkGpu("misc");
	if (DefersRenderPasses()) {
		BeginPassBuffer(state);
	} else {
		RecordBeginRendering(state, false);
	}
	m_render_state = state;
	m_rendering    = true;
	m_pass_serial++;
}

void CommandBuffer::BeginPassBuffer(const RenderState& state) const {
	EXIT_IF(m_pass_buffer != nullptr);
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> formats {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		formats[i] = state.color_attachments[i].format;
	}
	vk::CommandBufferInheritanceRenderingInfo rendering {};
	rendering.colorAttachmentCount    = state.num_color_attachments;
	rendering.pColorAttachmentFormats = formats.data();
	rendering.depthAttachmentFormat   = state.depth_format;
	rendering.stencilAttachmentFormat = state.stencil_format;
	rendering.rasterizationSamples    = vulkan_sample_count(state.samples);
	EXIT_IF(rendering.rasterizationSamples == vk::SampleCountFlagBits {});
	vk::CommandBufferInheritanceInfo inheritance {};
	inheritance.pNext = &rendering;
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit |
	              vk::CommandBufferUsageFlagBits::eRenderPassContinue;
	begin.pInheritanceInfo = &inheritance;
	m_pass_buffer          = m_scheduler.CommitPassBuffer();
	EXIT_IF(m_pass_buffer == nullptr);
	const auto result = m_pass_buffer.begin(&begin);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	// Dynamic state does not carry into a secondary command buffer.
	m_scheduler.GetDynamicState().Invalidate();
}

void CommandBuffer::RecordBeginRendering(const RenderState& state, bool secondary) const {
	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	if (secondary) {
		rendering.flags = vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;
	}
	Handle().beginRendering(rendering);
	Common::PerfStats::Count(Common::PerfStats::Counter::RenderPasses);
}

// KYTY_PASS_BARRIER=1 (opt-in): orders every finished render pass before everything recorded
// after it (later passes, compute dispatches incl. emulated mesh stages and indirect fix-ups,
// copies), like the fenced pass breadcrumbs of round 5 without the fill. Not proven to cure the
// crash-landing device loss: the narrower graphics -> compute variant still lost it (r6_c2_fix),
// and the one run of this form was cut by the thermal guard.
void CommandBuffer::OrderPassBeforeLaterWork() const {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PASS_BARRIER");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	if (!enabled) {
		return;
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite |
	                        vk::AccessFlagBits::eColorAttachmentWrite |
	                        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics,
	                         vk::PipelineStageFlagBits::eAllCommands, {}, 1, &barrier, 0, nullptr,
	                         0, nullptr);
	Common::PerfStats::Count(Common::PerfStats::Counter::GraphicsComputeBarriers);
}

void CommandBuffer::EndRendering(PassEnd reason) const {
	if (!m_rendering) {
		return;
	}
	if (m_pass_draws != 0 && Common::PerfStats::Enabled()) {
		using Common::PerfStats::Counter;
		static constexpr std::array kReasonCounters {
		    Counter::PassEndState,  Counter::PassEndTransit,     Counter::PassEndAttach,
		    Counter::PassEndImageCopy, Counter::PassEndBuffer,   Counter::PassEndCompute,
		    Counter::PassEndShaderWrite, Counter::PassEndSubmit, Counter::PassEndOther};
		static_assert(kReasonCounters.size() == static_cast<size_t>(PassEnd::Other) + 1);
		Common::PerfStats::Count(kReasonCounters[static_cast<size_t>(reason)]);
	}
	if (m_pass_buffer != nullptr) {
		EXIT_NOT_IMPLEMENTED(m_pass_buffer.end() != vk::Result::eSuccess);
		RecordBeginRendering(m_render_state, true);
		Handle().executeCommands(1, &m_pass_buffer);
		m_pass_buffer = nullptr;
	}
	Handle().endRendering();
	if (m_timer.Enabled()) {
		m_timer.Mark(Handle(), "pass", m_pass_shader,
		             {.width   = m_render_state.width,
		              .height  = m_render_state.height,
		              .colors  = m_render_state.num_color_attachments,
		              .depth   = m_render_state.depth_stencil_attachment.image_view != nullptr,
		              .draws   = m_pass_draws,
		              .indices = m_pass_indices});
	}
	m_pass_shader  = 0;
	m_pass_draws   = 0;
	m_pass_indices = 0;
	m_rendering    = false;
	m_render_state = {};
	OrderPassBeforeLaterWork();
}

} // namespace Libs::Graphics
