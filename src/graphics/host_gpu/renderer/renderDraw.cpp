#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/host_gpu/renderer/demonsRollingProbe.h"
#include "graphics/host_gpu/renderer/demonsAdaptiveCapture.h"
#include "graphics/host_gpu/renderer/rpStats.h"
#include "graphics/host_gpu/renderer/ufcPerf.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/renderer/gpuPipeline.h"
#include "graphics/host_gpu/renderer/gpuPipelineItems.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanDebugLabel.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/colorOutputRecovery.h"
#include "graphics/host_gpu/renderer/colorWritePolicy.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/renderScale.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/gpuTimer.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderIndirect.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "loader/systemContent.h"
#include "libs/errno.h"

#include <vulkan/vulkan_format_traits.hpp>

#include <algorithm>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// Draw accounting, enabled only for PPSA01341's already-present frame trace.
// The current screenshots contain UI but zero 3D scene: normal flip and
// shader compile counters do not say whether graphics draws got a real
// pipeline and color/depth attachments. This samples actual decisions and
// records only once per 8192 prepared draws, never a per-draw log.
static void TraceDsGraphicsOutcome(bool shader_ready, bool pipeline_ready,
                                   uint32_t colors, bool depth, bool ps_active) {
    static const bool enabled = [] {
        const auto* trace = std::getenv("KYTY_DS_FRAME_TRACE");
        return trace && trace[0] == '1' && trace[1] == '\0';
    }();
    if (!enabled) return;
    static std::atomic_uint64_t total {0}, no_shader {0}, no_pipeline {0};
    static std::atomic_uint64_t no_color {0}, no_depth {0}, accepted {0};
    static std::atomic_uint64_t no_ps {0}, depth_only {0};
    if (!shader_ready) no_shader.fetch_add(1, std::memory_order_relaxed);
    else if (!pipeline_ready) no_pipeline.fetch_add(1, std::memory_order_relaxed);
    else {
        accepted.fetch_add(1, std::memory_order_relaxed);
        if (colors == 0) no_color.fetch_add(1, std::memory_order_relaxed);
        if (!depth) no_depth.fetch_add(1, std::memory_order_relaxed);
        if (!ps_active) no_ps.fetch_add(1, std::memory_order_relaxed);
        if (colors == 0 && depth) depth_only.fetch_add(1, std::memory_order_relaxed);
    }
    const auto count = total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % 8192 != 0) return;
    Log::WriteToConsoleAndLog(fmt::format(
        "[DS_GRAPHICS_PATH] draws={} accepted={} shader_not_ready={} "
        "pipeline_null={} colorless={} depthless={} depth_only={} no_ps={}\n",
        count, accepted.load(std::memory_order_relaxed),
        no_shader.load(std::memory_order_relaxed),
        no_pipeline.load(std::memory_order_relaxed),
        no_color.load(std::memory_order_relaxed),
        no_depth.load(std::memory_order_relaxed),
        depth_only.load(std::memory_order_relaxed),
        no_ps.load(std::memory_order_relaxed)));
}

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.fetch_embedded) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

// The guest scissor of viewport `index` in host pixels of the pass (--render-scale shrinks it
// with the attachments).
static ScissorRect HostScissor(const HW::Context& ctx, const RenderState& rendering,
                               uint32_t index) {
	const auto guest = calc_final_scissor(ctx.GetScreenViewport(), ctx.GetScanModeControl(),
	                                      {rendering.guest_width, rendering.guest_height}, index);
	if (!RenderScale::Active(rendering.scale)) {
		return guest;
	}
	const auto host = RenderScale::ScaleRect({guest.left, guest.top, guest.right, guest.bottom},
	                                         rendering.scale, rendering.width, rendering.height);
	return {host.left, host.top, host.right, host.bottom};
}

static void UpdateGraphicsDynamicState(const CommandBuffer& buffer, DynamicState& dynamic_state,
                                       const ShaderVertexInputInfo& vs_input_info,
                                       const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
			if (RenderScale::Active(rendering.scale)) {
				viewport = RenderScale::ScaleViewport(viewport, rendering.scale);
			}
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor = HostScissor(ctx, rendering, i);
		auto& scissor  = scissors[i];
		scissor.offset = vk::Offset2D {final_scissor.left, final_scissor.top};
		scissor.extent = vk::Extent2D {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = vk::Extent2D {0, 0};
		}
	}
	dynamic_state.SetViewports(std::span {viewports}.first(viewport_count));
	dynamic_state.SetScissors(std::span {scissors}.first(viewport_count));

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	dynamic_state.SetLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	dynamic_state.SetBlendConstants(blend_constants);
	dynamic_state.SetDepthTestEnabled(depth.depth_test_enable);
	dynamic_state.SetDepthWriteEnabled(depth.depth_write_enable);
	dynamic_state.SetDepthCompareOp(depth.depth_compare_op);
	if (buffer.GetGraphics().optional_features.depth_bounds) {
		dynamic_state.SetDepthBoundsTestEnabled(depth.depth_bounds_test_enable);
		if (depth.depth_bounds_test_enable) {
			dynamic_state.SetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
		}
	} else if (depth.depth_bounds_test_enable) {
		// Fallback: the guest depth-bounds range is not applied, so fragments outside it are kept.
		static std::atomic<bool> s_warned = false;
		if (!s_warned.exchange(true)) {
			LOGF("WARNING: depth bounds test requested but depthBounds is unsupported; "
			     "the test is skipped\n");
		}
	}

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	dynamic_state.SetDepthBiasEnabled(depth_bias_enable);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		dynamic_state.SetDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	dynamic_state.SetStencilTestEnabled(depth.stencil_test_enable);
	if (depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		dynamic_state.SetStencilOps({front.failOp, front.passOp, front.depthFailOp, front.compareOp},
		                            {back.failOp, back.passOp, back.depthFailOp, back.compareOp});
		dynamic_state.SetStencilCompareMasks(front.compareMask, back.compareMask);
		dynamic_state.SetStencilWriteMasks(front.writeMask, back.writeMask);
		dynamic_state.SetStencilReferences(front.reference, back.reference);
	}

	// Without VK_EXT_color_write_enable the pipeline has no eColorWriteEnableEXT dynamic state and
	// relies on the static colorWriteMask instead.
	if (buffer.GetGraphics().optional_features.color_write_enable) {
		vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
		const auto shader_mask = ctx.GetShaderRegisters().m_cbShaderMask;
		const auto target_mask = ctx.GetRenderTargetMask();
		for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
			enable[slot] = GuestColorWriteEnabled(
			    target_mask, shader_mask, slot,
			    rendering.color_attachments[slot].image_view != nullptr) ? VK_TRUE : VK_FALSE;
		}
		dynamic_state.SetColorWriteEnables(
		    std::span {enable}.first(rendering.num_color_attachments));
	}
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop || sh_regs.AlphaToMask();
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
	ShaderIndirectDrawInfo indirect; // --gpu-indirect draws
	// The renderer serializes draws under its mutex. Reuse this large scratch
	// (Jetsku U59), rather than value-initializing tens of KB per draw.
	uint32_t previous_vertex_stages = 0;
	void ResetForNextDraw() {
		depth_info = {};
		for (auto& target : color_info) target = {};
		color_count = 0;
		ps_active = true;
		for (uint32_t stage = 1; stage < previous_vertex_stages; ++stage)
			vertex_info[stage] = {};
		previous_vertex_stages = 0;
		// The shader preparation reconstructs vertex[0], pixel and programs.
		// Never retain prior indirect guest SGPRs across an ordinary draw.
		indirect = {};
	}
};

static DrawRenderState& ReusableDrawRenderState() {
	static thread_local DrawRenderState state {};
	state.ResetForNextDraw();
	return state;
}

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

// The render area must fit the attachment view: a mip view or a one-texel alias can be smaller
// than the guest target's extent.
static vk::Extent2D AttachmentViewExtent(const Image& image, const ImageViewInfo& view) {
	return {std::max(image.backing.extent.width >> view.base_level, 1u),
	        std::max(image.backing.extent.height >> view.base_level, 1u)};
}

void RenderExecutor::NoteAttachmentPassEnd(std::span<const RenderColorInfo>   colors,
                                           const RenderDepthInfo&             depth,
                                           std::span<const VulkanImageState> old_states) {
	std::array<ImageId, RENDER_COLOR_ATTACHMENTS_MAX + 1> images {};
	uint32_t                                              count = 0;
	for (const auto& color: colors) {
		images[count++] = color.image_id;
	}
	if (depth.image_id) {
		images[count++] = depth.image_id;
	}
	const bool same = count == m_pass_image_count &&
	                  std::equal(images.begin(), images.begin() + count, m_pass_images.begin());
	if (same) {
		Common::PerfStats::Count(Common::PerfStats::Counter::PassEndAttachSame);
	}
	// Log a sample from the steady state (the first events are the title/loading screens).
	static const auto           started = std::chrono::steady_clock::now();
	static std::atomic_uint32_t logged  = 0;
	if (std::chrono::steady_clock::now() - started < std::chrono::seconds(95) ||
	    logged.fetch_add(1, std::memory_order_relaxed) >= 64) {
		return;
	}
	auto&       cache = m_context.GetTextureCache();
	std::string line  = same ? "[attach-end] same images:" : "[attach-end] old=[";
	if (!same) {
		for (uint32_t i = 0; i < m_pass_image_count; i++) {
			line += fmt::format(" {}", m_pass_images[i].index);
		}
		line += " ] new:";
	}
	for (uint32_t i = 0; i < count; i++) {
		const auto* image = cache.m_slot_images.try_get(images[i]);
		const auto& old_state = old_states[i];
		line += fmt::format(" {}({}x{} {}/{:x} -> {}/{:x})", images[i].index,
		                    image != nullptr ? image->info.extent.width : 0,
		                    image != nullptr ? image->info.extent.height : 0,
		                    vk::to_string(old_state.layout),
		                    static_cast<uint64_t>(old_state.access_mask),
		                    image != nullptr ? vk::to_string(image->backing.state.layout) : "?",
		                    image != nullptr
		                        ? static_cast<uint64_t>(image->backing.state.access_mask)
		                        : 0);
	}
	LOGF("%s\n", line.c_str());
}

void RenderExecutor::MarkPassImages(const CommandBuffer&               buffer,
                                    std::span<PreparedBindings* const> stages,
                                    std::span<const RenderColorInfo>   colors,
                                    const RenderDepthInfo&             depth) {
	m_pass_image_count = 0;
	for (const auto& color: colors) {
		m_pass_images[m_pass_image_count++] = color.image_id;
	}
	if (depth.image_id) {
		m_pass_images[m_pass_image_count++] = depth.image_id;
	}
	if (!buffer.HoistsBarriers()) {
		return;
	}
	auto&      cache  = m_context.GetTextureCache();
	const auto serial = buffer.PassSerial();
	const auto mark   = [&](ImageId id) {
		if (auto* image = cache.m_slot_images.try_get(id); image != nullptr) {
			image->pass_serial = serial;
		}
	};
	for (const auto* stage: stages) {
		for (const auto& binding: stage->images) {
			if (binding.image_id) {
				mark(binding.image_id);
			}
		}
	}
	for (const auto& color: colors) {
		mark(color.image_id);
	}
	if (depth.image_id) {
		mark(depth.image_id);
	}
}

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.guest_width           = std::numeric_limits<uint32_t>::max();
	state.guest_height          = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	// --perf-stats: attachment transitions below may end a pass with draws; note why.
	const bool pass_open = Common::PerfStats::Enabled() && buffer.IsRendering() &&
	                       buffer.PassDraws() != 0;
	std::array<VulkanImageState, RENDER_COLOR_ATTACHMENTS_MAX + 1> old_states {};
	// --render-scale: every attachment of a pass shares one size. The pass is scaled when all
	// of them can be; otherwise the scalable ones return to guest size for good (a partner that
	// cannot scale means this is not a plain frame-sized pass).
	// A square high-resolution GT depth atlas is a depth-only pass.
	// Its scaled backing needs a matching viewport/scissor/push-data ratio;
	// never mix its extra 1/2 scaling with color attachments using the
	// ordinary --render-scale ratio.
	const auto* depth_owner = depth.image_id ? cache.m_slot_images.try_get(depth.image_id) : nullptr;
	const bool gt_depth_half_pass =
	    color_count == 0 && depth_owner != nullptr && depth_owner->gt_large_depth_half;
	const bool gt_hdr_half_pass =
	    color_count == 1 && !depth.image_id &&
	    cache.m_slot_images.try_get(colors[0].image_id) != nullptr &&
	    cache.m_slot_images[colors[0].image_id].gt_hdr_half;
	// The pixel shader's guest/host transform must match the actual depth
	// backing. A square shadow atlas uses the global ratio; the optional GT
	// experiment may have its own (half) ratio.
	const float render_scale = gt_depth_half_pass
	    ? depth_owner->render_scale_factor
	    : Config::GetRenderScale() * (gt_hdr_half_pass ? 0.5f : 1.0f);
	bool pass_scaled = RenderScale::Active(render_scale) &&
	                   (color_count != 0 || static_cast<bool>(depth.image_id));
	// Multi-attachment passes cannot mix host backings with different
	// dimensions or shader coordinate scales. Restore guest-sized backings
	// instead of risking a framebuffer/viewport mismatch.
	if ((color_count != 0 && depth_owner && depth_owner->gt_large_depth_half) ||
	    ((color_count != 1 || depth.image_id) &&
	     std::any_of(colors, colors + color_count, [&](const RenderColorInfo& color) {
		     const auto* img = cache.m_slot_images.try_get(color.image_id);
		     return img && img->gt_hdr_half;
	     }))) {
		pass_scaled = false;
	}
	for (uint32_t i = 0; i < color_count && pass_scaled; i++) {
		const auto owner = cache.m_slot_images.try_get(colors[i].image_id);
		pass_scaled      = owner != nullptr && owner->CanScale();
	}
	if (pass_scaled && depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		pass_scaled      = owner != nullptr && owner->CanScale();
	}
	const auto apply_scale = [&](ImageId id) {
		auto& image = cache.GetImage(id);
		if (pass_scaled) {
			image.SetScaled(true);
		} else if (image.scalable) {
			image.BlockScaling();
		}
	};
	if (RenderScale::Active(render_scale)) {
		// Guest refreshes (uploads) run at guest size; do them before the pass size is applied
		// so the attachment views below are made on the final backing.
		for (uint32_t i = 0; i < color_count; i++) {
			cache.UpdateImage(colors[i].image_id);
		}
		if (depth.image_id) {
			cache.UpdateImage(depth.image_id);
		}
		for (uint32_t i = 0; i < color_count; i++) {
			apply_scale(colors[i].image_id);
		}
		if (depth.image_id) {
			apply_scale(depth.image_id);
		}
	}
	// An acquisition below that still moved the image (a refresh or download of an alias) is
	// undone here, with a view on the final backing.
	const auto ensure_pass_scale = [&](Image& image, const ImageViewInfo& view,
	                                   vk::ImageView& image_view) {
		if (image.scaled == pass_scaled) {
			return;
		}
		static std::atomic_uint32_t s_logged = 0;
		if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("render scale: attachment 0x%016" PRIx64 " changed size during acquisition "
			     "(scaled=%d blocked=%d pass=%d)\n",
			     image.info.data.address, image.scaled, image.scale_blocked, pass_scaled);
		}
		image.SetScaled(pass_scaled);
		EXIT_IF(image.scaled != pass_scaled);
		image_view = image.FindView(view);
	};
	m_pass_scaled = pass_scaled;
	state.scale   = pass_scaled ? render_scale : 1.0f;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		auto  image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto& image      = cache.GetImage(target.image_id);
		ensure_pass_scale(image, target.desc.view_info, image_view);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto shader = stages.empty() ? 0u : stages.back()->runtime->program->shader_hash;
		RememberDsAtlasWriter(target.image_id, target.desc.info, view, shader, "attachment");
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		old_states[i]     = image.backing.state;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		{
			// KYTY_RP_OPS: becoming this pass's attachment keeps a folded clear pending.
			const Image::FoldScope fold_scope;
			image.Transit(layout, image.binding.attachment_access,
			              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
			                                     view.layer_count},
			              buffer.Handle(), PassEnd::Attach);
		}
		const auto extent       = target.Extent();
		const auto view_extent  = AttachmentViewExtent(image, view);
		state.width             = std::min(state.width, view_extent.width);
		state.height            = std::min(state.height, view_extent.height);
		state.guest_width       = std::min(state.guest_width, extent.width);
		state.guest_height      = std::min(state.guest_height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
		attachment.format       = view.format;
		state.samples           = target.desc.info.samples;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		auto        image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || meta_clear;
		// HTile also carries Hi-Stencil when that plane is compressed. A
		// pending metadata clear must materialize BOTH aspects; otherwise stale
		// stencil from the previous 3D pass survives the next draw (PC 5bf37f5).
		depth.stencil_clear_enable |= meta_clear && metadata.stencil_compressed;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		ensure_pass_scale(image, depth.desc.view_info, image_view);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		// Scaling the attachments above may have swapped this image's backing after the draw's
		// texture bindings resolved their views (RebindImages).
		RefreshBindingViews(stages);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		const auto per_fragment_writes = depth.AttachmentWriteAspects(false);
		if ((feedback_aspects & per_fragment_writes) &&
		    !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("depth attachment feedback loop is not supported by the host\n");
			}
		}
		// A depth image the draw does not sample stays in a writable layout, so draws that only
		// toggle depth/stencil writes share one render pass (legacy: --layout-mode 0).
		auto layout = !sampled_aspects && Config::GetLayoutMode() >= 1
		                  ? depth_writable_attachment_layout(depth)
		                  : depth_attachment_layout(depth);
		old_states[color_count] = image.backing.state;
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		              vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		// A draw that samples aspects it does not write (depth test on, depth write off) needs
		// no barrier against the previous draw: request the sampled access here as well, so the
		// descriptor transition (PrepareBindingWrites, is_target) finds the state it asks for and
		// consecutive such draws share one render pass. Real feedback (sampled aspects written)
		// keeps the legacy per-draw barrier.
		if (sampled_aspects && !feedback_aspects && Config::GetLayoutMode() >= 1) {
			access |= vk::AccessFlagBits2::eShaderRead;
		}
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle(), PassEnd::Attach);
		const auto view_extent    = AttachmentViewExtent(image, view);
		state.width               = std::min(state.width, view_extent.width);
		state.height              = std::min(state.height, view_extent.height);
		state.guest_width         = std::min(state.guest_width, depth.desc.info.extent.width);
		state.guest_height        = std::min(state.guest_height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
		attachment.format         = view.format;
		state.depth_format        = attachment.has_depth ? view.format : vk::Format::eUndefined;
		state.stencil_format      = attachment.has_stencil ? view.format : vk::Format::eUndefined;
		state.samples             = depth.desc.info.samples;
	}
	if (pass_open && !buffer.IsRendering()) {
		NoteAttachmentPassEnd(std::span {colors, color_count}, depth, old_states);
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
		state.guest_width  = state.width;
		state.guest_height = state.height;
		// Matches the pipeline's rasterization sample count for attachment-less draws.
		state.samples =
		    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
	}
	if (state.samples == 0) {
		state.samples = 1;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	// KYTY_RP_OPS: a folded clear becomes this pass's loadOp CLEAR when the pass covers the whole
	// image in the image's own format (the clear value means the same); otherwise it is recorded
	// now and the attachment acquired again.
	for (uint32_t i = 0; i < color_count; i++) {
		auto* image = cache.m_slot_images.try_get(colors[i].image_id);
		if (image == nullptr || !image->rp_fold.Pending()) continue;
		auto&       attachment = state.color_attachments[colors[i].target_slot];
		const auto& view       = colors[i].desc.view_info;
		const RpFold::PassUse use {.pass_width      = state.width,
		                           .pass_height     = state.height,
		                           .pass_layers     = state.num_layers,
		                           .view_base_level = view.base_level,
		                           .image_width     = image->backing.extent.width,
		                           .image_height    = image->backing.extent.height,
		                           .same_format     = attachment.format == image->backing.format};
		vk::ClearColorValue value {};
		if (image->rp_fold.TryAbsorb(use, value)) {
			attachment.is_clear = true;
			std::memcpy(attachment.clear_value.data(), &value, sizeof(attachment.clear_value));
			RpStats::OnFolded();
		} else {
			image->FlushFoldedClear();
			image->Transit(attachment.image_layout, image->binding.attachment_access,
			               ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
			                                      view.layer_count},
			               buffer.Handle(), PassEnd::Attach);
		}
	}
	if (RpStats::Enabled()) {
		// A clear still pending on a colour target would be absorbed by loadOp CLEAR when the
		// pass covers the whole image (the pass state below is its render area).
		for (uint32_t i = 0; i < color_count; i++) {
			auto* image = cache.m_slot_images.try_get(colors[i].image_id);
			if (image == nullptr || !image->rp_clear_pending) continue;
			image->rp_clear_pending = false;
			const bool full = state.width == image->backing.extent.width &&
			                  state.height == image->backing.extent.height;
			RpStats::OnAttachment(full, image->info.data.size);
		}
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max() ||
	        state.guest_width == std::numeric_limits<uint32_t>::max() ||
	        state.guest_height == std::numeric_limits<uint32_t>::max());
	return state;
}

// PPSA01341: bounded *GPU pixel* validation, not another draw/pipeline counter.
// On every 8192nd accepted color draw, sample 5 actual pixels of up to three
// render-target images. This distinguishes an all-black G-buffer from an
// all-black final composition (their addresses, shader hashes and formats are
// reported separately). The readback is synchronized and never changes guest
// image ownership or writes guest memory. Other games never enable it.
static bool DsBlockCompressed(vk::Format format) {
	return format >= vk::Format::eBc1RgbUnormBlock && format <= vk::Format::eBc7SrgbBlock;
}

static void ProbeDsColorOutputs(RenderContext& context, const RenderColorInfo* colors,
                                uint32_t count, uint64_t shader_hash, bool compute = false, bool forced = false,
                                const ImageViewInfo* sampled_view = nullptr,
                                const DsProbePixels* pixels = nullptr,
                                uint32_t reference_width = 0, uint32_t reference_height = 0,
                                std::array<uint32_t, DsProbePixels{}.size()>* words = nullptr) {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DS_COLOR_PROBE");
		// An explicit 0 disables probing. Otherwise reuse the existing DS
		// frame trace opt-in on Android; no extra launcher setting required.
		if (value != nullptr) return value[0] == '1' && value[1] == '\0';
		value = std::getenv("KYTY_DS_FRAME_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	const uint32_t kind = compute ? 1u : 0u;
	static std::atomic_bool setup_logged[2] {};
	if (!setup_logged[kind].exchange(true, std::memory_order_relaxed)) {
		const char* explicit_probe = std::getenv("KYTY_DS_COLOR_PROBE");
		const char* trace = std::getenv("KYTY_DS_FRAME_TRACE");
		Log::WriteToConsoleAndLog(fmt::sprintf("[DS_PIXEL_PROBE_SETUP] path=%s enabled=%u count=%u probe=%s trace=%s\n",
		     compute ? "compute" : "graphics", enabled ? 1u : 0u, count,
		     explicit_probe ? explicit_probe : "(unset)", trace ? trace : "(unset)"));
	}
	if (!enabled || count == 0) return;
	// The Demon launcher enables a strict two-phase compositor comparison,
	// not the earlier 48 color + 128 compute periodic GPU stalls.
	static const bool compositor_only = [] {
		const char* v = std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");
		return v != nullptr && v[0] == '1' && v[1] == '\0';
	}();
	if (compositor_only && !forced) return;
	static std::atomic_uint64_t eligible_events[2] {};
	static std::atomic_uint32_t captures[2] {};
	const uint32_t period = compute ? 256u : 4096u;
	const uint32_t limit = compute ? 128u : 48u;
	const uint64_t event = eligible_events[kind].fetch_add(1, std::memory_order_relaxed) + 1u;
	// Targeted probes are already throttled by each rolling caller. Do not
	// exhaust a fixed run-wide budget or require a minimum guest frame.
	if (!compositor_only &&
	    ((!forced && event % period != 0u) ||
	     captures[kind].load(std::memory_order_relaxed) >= limit)) return;
	auto& cache = context.GetTextureCache();
	auto& scheduler = context.GetCommandScheduler();
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	const auto bytes_per_pixel = [](vk::Format format) -> uint32_t {
		switch (format) {
			case vk::Format::eR8G8B8A8Unorm:
			case vk::Format::eR32Uint:
			case vk::Format::eR32Sint:
			case vk::Format::eR8G8B8A8Srgb:
			case vk::Format::eR8G8B8A8Uint:
			case vk::Format::eR8G8B8A8Snorm:
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eB8G8R8A8Srgb:
			case vk::Format::eB10G11R11UfloatPack32:
			case vk::Format::eR16G16Sfloat:
			case vk::Format::eR16G16Unorm:
			case vk::Format::eR16G16Snorm:
			case vk::Format::eR32Sfloat:
			case vk::Format::eA2B10G10R10UnormPack32:
			case vk::Format::eA2R10G10B10UnormPack32: return 4;
			case vk::Format::eR16G16B16A16Sfloat:
			case vk::Format::eR16G16B16A16Unorm:
			case vk::Format::eR16G16B16A16Snorm:
			case vk::Format::eR16G16B16A16Uint:
			case vk::Format::eR16G16B16A16Sint:
			case vk::Format::eR32G32Sfloat: return 8;
			case vk::Format::eR32G32B32A32Sfloat: return 16;
			case vk::Format::eR16Sfloat:
			case vk::Format::eR8G8Unorm:
			case vk::Format::eR8G8Snorm: return 2;
			case vk::Format::eR8Uint:
			case vk::Format::eR8Unorm: return 1;
			// BC1 is sampled in *compressed 4x4 blocks*; never copy a 1x1 texel.
			case vk::Format::eBc1RgbaSrgbBlock:
			case vk::Format::eBc1RgbaUnormBlock:
			case vk::Format::eBc1RgbSrgbBlock:
			case vk::Format::eBc1RgbUnormBlock: return 8;
			default: return 0;
		}
	};
	const uint32_t capture = captures[kind].fetch_add(1, std::memory_order_relaxed) + 1u;
	uint32_t probed = 0, no_id = 0, no_backing = 0, not_1x = 0;
	uint32_t unknown_format = 0, tiny = 0, no_transfer = 0;
	uint32_t example_format = 0, example_width = 0, example_height = 0;
	for (uint32_t slot = 0; slot < count && probed < 3u; ++slot) {
		if (!colors[slot].image_id) { ++no_id; continue; }
		auto& image = cache.GetImage(colors[slot].image_id);
		const auto& backing = image.backing;
		const auto& view = sampled_view != nullptr ? *sampled_view : colors[slot].desc.view_info;
		const uint32_t view_layer = view.base_layer;
		const uint32_t view_mip = view.base_level;
		const auto depth_bytes = DepthAspectTransferBytes(backing.format);
		const auto available = depth_bytes != 0 ? ImageViewOps::DepthAspectMask(backing.format)
		    : vk::ImageAspectFlags {vk::ImageAspectFlagBits::eColor};
		const auto aspect = sampled_view != nullptr ? view.aspect
		    : vk::ImageAspectFlags {depth_bytes != 0 ? vk::ImageAspectFlagBits::eDepth
		                                           : vk::ImageAspectFlagBits::eColor};
		const bool depth = aspect == vk::ImageAspectFlagBits::eDepth;
		const bool stencil = aspect == vk::ImageAspectFlagBits::eStencil;
		const bool compressed = DsBlockCompressed(backing.format);
		const uint32_t bpp = stencil ? 1u : depth ? depth_bytes
		                   : compressed ? vk::blockSize(backing.format) : bytes_per_pixel(backing.format);
		if (example_format == 0u) {
			example_format = static_cast<uint32_t>(backing.format);
			example_width = backing.extent.width; example_height = backing.extent.height;
		}
		if (!backing.image) { ++no_backing; continue; }
		if (backing.samples != 1u) { ++not_1x; continue; }
		if (bpp == 0u || (aspect & ~available) ||
		    (!depth && !stencil && aspect != vk::ImageAspectFlagBits::eColor)) {
			++unknown_format;
			continue;
		}
		if ((!compressed && (backing.extent.width < 4u || backing.extent.height < 4u)) ||
		    backing.image_type != vk::ImageType::e2D) { ++tiny; continue; }
		if (!(backing.usage & vk::ImageUsageFlagBits::eTransferSrc)) { ++no_transfer; continue; }
		// Sample precisely the native view bound to the shader, not the owner image's
		// first layer. Some PS5 texture descriptors alias a nonzero depth array slice.
		if (view_layer >= backing.layers || view_mip >= backing.mip_levels) {
			Log::WriteToConsoleAndLog(fmt::sprintf(
			    "[DS_PIXEL_PROBE_REJECT] path=%s shader=0x%016" PRIx64
			    " layer=%u/%u mip=%u/%u reason=invalid_subresource\n",
			    compute ? "compute" : "graphics", shader_hash,
			    view_layer, backing.layers, view_mip, backing.mip_levels));
			continue;
		}
		++probed;
		// Vulkan depth/stencil buffer copies require DWORD-aligned offsets, including S8.
		const uint32_t stride = depth || stencil ? std::max(bpp, 4u) : bpp;
		const uint64_t length = stride * 5u;
		auto [mapped, offset] = download.Map(length, 16u, true);
		if (mapped == nullptr) {
			Log::WriteToConsoleAndLog(fmt::sprintf("[DS_PIXEL_PROBE] path=%s shader=0x%016" PRIx64 " slot=%u result=staging_unavailable\n",
			     compute ? "compute" : "graphics", shader_hash, slot));
			continue;
		}
		download.Commit();
		const uint32_t w = std::max(backing.extent.width >> view_mip, 1u);
		const uint32_t h = std::max(backing.extent.height >> view_mip, 1u);
		const DsProbePixels xy = pixels ? *pixels : DemonsQueueProbe::DefaultPixels(w, h);
		std::array<vk::BufferImageCopy,5> regions {};
		for (size_t i = 0; i < regions.size(); ++i) {
			regions[i].bufferOffset = offset + i * stride;
			regions[i].imageSubresource.aspectMask = aspect;
			regions[i].imageSubresource.mipLevel = view_mip;
			regions[i].imageSubresource.baseArrayLayer = view_layer;
			regions[i].imageSubresource.layerCount = 1;
			const auto x = std::min<uint64_t>(
			    reference_width ? uint64_t{xy[i].first} * w / reference_width : xy[i].first,
			    w - 1u);
			const auto y = std::min<uint64_t>(
			    reference_height ? uint64_t{xy[i].second} * h / reference_height : xy[i].second,
			    h - 1u);
			const auto bx = compressed ? DemonsQueueProbe::SampleBlock(x, w) :
			    DemonsQueueProbe::BlockAxis {static_cast<uint32_t>(x), 1u};
			const auto by = compressed ? DemonsQueueProbe::SampleBlock(y, h) :
			    DemonsQueueProbe::BlockAxis {static_cast<uint32_t>(y), 1u};
			regions[i].imageOffset = vk::Offset3D{
			    static_cast<int32_t>(bx.offset), static_cast<int32_t>(by.offset), 0};
			regions[i].imageExtent = vk::Extent3D{bx.extent, by.extent, 1};
		}
		image.Download(regions, download.Handle(), offset, length);
		// A once-per-4096-draw diagnostic GPU wait is intentional. Do not read
		// bytes until the command containing vkCmdCopyImageToBuffer completed.
		scheduler.Finish();
		download.Invalidate(offset, length);
		uint32_t zero = 0, zero_rgb = 0;
		std::array<uint32_t,5> first_words {}, second_words {};
		std::array<std::array<uint32_t, 4>, 5> blocks {};
		for (uint32_t i = 0; i < 5; ++i) {
			const auto* pixel = mapped + i * stride;
			bool empty = true, rgb_black = true;
			for (uint32_t b = 0; b < bpp; ++b) {
				empty &= pixel[b] == 0u;
				// RGBA16F: RGB are bytes [0,6); alpha alone is not visible lighting.
				if (b < (backing.format == vk::Format::eR16G16B16A16Sfloat ? 6u : bpp))
					rgb_black &= pixel[b] == 0u;
			}
			zero += empty;
			std::memcpy(&first_words[i], pixel, std::min(bpp, 4u));
			if (backing.format == vk::Format::eA2B10G10R10UnormPack32 ||
			    backing.format == vk::Format::eA2R10G10B10UnormPack32)
				rgb_black = DemonsQueueProbe::PackedRgb10IsBlack(first_words[i]);
			if (backing.format == vk::Format::eR32Uint &&
			    image.info.data.address == DemonsQueueProbe::GBufferAddress && view_layer == 0)
				rgb_black = (first_words[i] & 0x00ffffffu) == 0;
			zero_rgb += rgb_black;
			if (compressed) std::memcpy(blocks[i].data(), pixel, bpp);
			if (bpp > 4u)
				std::memcpy(&second_words[i], pixel + 4u, std::min(bpp - 4u, 4u));
		}
		Log::WriteToConsoleAndLog(fmt::sprintf("[DS_PIXEL_PROBE] path=%s capture=%u shader=0x%016" PRIx64
		     " frame=%d slot=%u guest=0x%012" PRIx64 " format=%u extent=%ux%u"
		     " layer=%u mip=%u samples=5 compressed_blocks=%u depth=%u aspect=0x%x zero=%u rgb_black=%u first=%08x,%08x,%08x,%08x,%08x"
		     " second=%08x,%08x,%08x,%08x,%08x\n",
		     compute ? "compute" : "graphics", capture, shader_hash,
		     context.GetGpu().GetFrameNum(), slot, image.info.data.address,
		     static_cast<uint32_t>(backing.format), w, h, view_layer, view_mip, compressed ? 1u : 0u,
		     depth ? 1u : 0u, static_cast<uint32_t>(aspect), zero, zero_rgb,
		     first_words[0],first_words[1],first_words[2],first_words[3],first_words[4],
		     second_words[0],second_words[1],second_words[2],second_words[3],second_words[4]));
		if (words) *words = first_words;
		if (compressed) {
			std::string words;
			for (const auto& block : blocks) {
				words += fmt::format(" {:08x}:{:08x}:{:08x}:{:08x}",
				                     block[0], block[1], block[2], block[3]);
			}
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_MATERIAL_BLOCKS] frame={} shader=0x{:016x} owner=0x{:x} format={} "
			    "mip={} layer={} block_bytes={} zero_blocks={} words={}\n",
			    context.GetGpu().GetFrameNum(), shader_hash, image.info.data.address,
			    static_cast<uint32_t>(backing.format), view_mip, view_layer, bpp, zero, words));
		}
	}
	if (probed == 0) {
		Log::WriteToConsoleAndLog(fmt::sprintf("[DS_PIXEL_PROBE_REJECT] path=%s capture=%u event=%" PRIu64
		     " shader=0x%016" PRIx64 " count=%u no_id=%u no_backing=%u msaa=%u"
		     " format=%u small=%u transfer=%u example_format=%u example_extent=%ux%u\n",
		     compute ? "compute" : "graphics", capture, event, shader_hash, count,
		     no_id, no_backing, not_1x, unknown_format, tiny, no_transfer,
		     example_format, example_width, example_height));
	}
}

// A full layer comparison also finds writes between the occlusion samples.
struct DsAlbedoSnapshot {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint32_t> words;
};

static DsAlbedoSnapshot SnapshotDsAlbedo(RenderContext& context, const RenderColorInfo& color) {
	auto& image = context.GetTextureCache().GetImage(color.image_id);
	const auto& backing = image.backing;
	const auto& view = color.desc.view_info;
	const uint64_t area = uint64_t{backing.extent.width} * backing.extent.height;
	if (!backing.image || backing.image_type != vk::ImageType::e2D || backing.samples != 1 ||
	    backing.format != vk::Format::eR32Uint || !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	    view.base_level != 0 || view.base_layer != 0 || area == 0 || area > 4u * 1024u * 1024u) {
		return {};
	}
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	const auto size = area * sizeof(uint32_t);
	auto [mapped, offset] = download.Map(size, 16u, true);
	if (!mapped) return {};
	download.Commit();
	vk::BufferImageCopy copy {};
	copy.bufferOffset = offset;
	copy.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor,
	                                                   view.base_level, view.base_layer, 1};
	copy.imageExtent = vk::Extent3D {backing.extent.width, backing.extent.height, 1};
	image.Download(std::span{&copy, 1u}, download.Handle(), offset, size);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, size);
	DsAlbedoSnapshot result {backing.extent.width, backing.extent.height, std::vector<uint32_t>(area)};
	std::memcpy(result.words.data(), mapped, size);
	return result;
}

static void RecordDsMaterialPixels(DemonsQueueProbe::MaterialTrace& trace,
                                   const DsAlbedoSnapshot& before,
                                   const DsAlbedoSnapshot& after,
                                   std::span<const DemonsQueueProbe::Pixels::value_type> points,
                                   std::span<const uint64_t> hits, bool cleared) {
	if (before.words.empty() || before.width != after.width || before.height != after.height) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_GBUFFER_PIXELS] frame={} shader=0x{:016x} result=snapshot_unavailable\n",
		    trace.frame, trace.shader));
		return;
	}
	trace.width = before.width;
	trace.height = before.height;
	uint32_t equal_count = 0, always_count = 0, changed = 0, colored_after = 0;
	const auto report = [&](DemonsQueueProbe::Pixels::value_type point, const char* evidence) {
		const auto used = std::span{trace.pixels.data(), trace.count};
		if (trace.count == trace.pixels.size() || std::ranges::find(used, point) != used.end())
			return;
		const auto [x, y] = point;
		const auto index = size_t{y} * trace.width + x;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_GBUFFER_PIXEL] frame={} shader=0x{:016x} sample={} xy={},{} "
		    "evidence={} before={:08x} after={:08x}\n",
		    trace.frame, trace.shader, trace.count, x, y, evidence,
		    before.words[index], after.words[index]));
		trace.output[trace.count] = after.words[index];
		trace.pixels[trace.count++] = point;
	};
	// Prefer depth-accepted samples whose color stayed black, then any other
	// depth-accepted sample. A changed word is independent evidence of a write.
	for (size_t i = 0; i < points.size(); ++i) {
		const auto [x, y] = points[i];
		const auto index = size_t{y} * trace.width + x;
		equal_count += hits[2 + 2 * i] != 0;
		always_count += hits[3 + 2 * i] != 0;
		if (hits[2 + 2 * i] && (after.words[index] & 0x00ffffffu) == 0)
			report(points[i], "equal_black");
	}
	for (size_t i = 0; i < points.size(); ++i) {
		if (hits[2 + 2 * i]) report(points[i], "equal");
	}
	for (size_t i = 0; i < after.words.size(); ++i) {
		colored_after += (after.words[i] & 0x00ffffffu) != 0;
		changed += before.words[i] != after.words[i];
	}
	// This is earlier, real evidence of rendered 3D geometry than the
	// periodic light-queue sample. The 718 capture showed it in the G-buffer
	// while the HDR monitor was bright, before the dark transition. Reuse the
	// actual material/depth coverage rather than guessing a gameplay frame.
	if (changed != 0 && colored_after != 0)
		DemonsAdaptiveCapture::Current().MarkGeometry(trace.frame, changed);
	if (changed && !cleared && trace.count < trace.pixels.size()) {
		const uint32_t step = std::max(changed / static_cast<uint32_t>(trace.pixels.size()), 1u);
		uint32_t ordinal = 0;
		for (size_t i = 0; i < after.words.size() && trace.count < trace.pixels.size(); ++i) {
			if (before.words[i] == after.words[i] || ordinal++ % step != 0) continue;
			report({static_cast<uint32_t>(i % trace.width), static_cast<uint32_t>(i / trace.width)},
			       "attachment_changed");
		}
	}
	// Keep the sampler's five lanes valid when the mesh covers fewer points.
	std::fill(trace.pixels.begin() + trace.count, trace.pixels.end(), trace.pixels[0]);
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_GBUFFER_PIXELS] frame={} shader=0x{:016x} probes={} equal={} always={} "
	    "changed={} cleared_on_load={} colored_after={} extent={}x{} traced={}\n",
	    trace.frame, trace.shader, points.size(), equal_count, always_count,
	    changed, cleared, colored_after, trace.width, trace.height, trace.count));
}

// Probe the HDR scene *actually consumed* by the final compositor.  The image
// is sampled at a bounded cadence, avoiding an assumed start-of-game frame.
static void MonitorDsCompositeLight(RenderContext& context, const PreparedBindings& pixel,
                                    int32_t frame) {
	static const bool ds_trace_enabled = [] {
		const auto* value = std::getenv("KYTY_DS_FRAME_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!ds_trace_enabled || !DemonsAdaptiveCapture::Current().ShouldSample(frame)) return;
	if (Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	const auto selected = std::ranges::find_if(pixel.images, [](const auto& binding) {
		return binding.image_id && !binding.placeholder &&
		       binding.desc.info.data.address == DemonsQueueProbe::SceneAddress;
	});
	if (selected == pixel.images.end()) return;
	auto& image = context.GetTextureCache().GetImage(selected->image_id);
	const auto& backing = image.backing;
	const auto view = std::ranges::find(image.views, selected->image_view,
	                                   &CachedImageView::view);
	if (view == image.views.end() || !backing.image || backing.samples != 1 ||
	    backing.image_type != vk::ImageType::e2D ||
	    backing.format != vk::Format::eR16G16B16A16Sfloat ||
	    !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	    view->info.base_level >= backing.mip_levels ||
	    view->info.base_layer >= backing.layers) return;
	const auto w = std::max(backing.extent.width >> view->info.base_level, 1u);
	const auto h = std::max(backing.extent.height >> view->info.base_level, 1u);
	if (w < 16 || h < 16) return;
	constexpr uint32_t count = 64;
	constexpr uint32_t texel_bytes = 8;
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(count * texel_bytes, 16u, true);
	if (!mapped) return;
	download.Commit();
	std::array<vk::BufferImageCopy, count> regions {};
	for (uint32_t i = 0; i < count; ++i) {
		auto& r = regions[i];
		r.bufferOffset = offset + uint64_t{i} * texel_bytes;
		r.imageSubresource = {vk::ImageAspectFlagBits::eColor,
		                      view->info.base_level, view->info.base_layer, 1};
		r.imageOffset = {static_cast<int32_t>((i % 8u * 2u + 1u) * w / 16u),
		                 static_cast<int32_t>((i / 8u * 2u + 1u) * h / 16u), 0};
		r.imageExtent = {1, 1, 1};
	}
	image.Download(std::span{regions}, download.Handle(), offset, count * texel_bytes);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, count * texel_bytes);
	const auto half = [](uint16_t value) -> double {
		if (value & 0x8000u || (value & 0x7c00u) == 0x7c00u) return 0.0;
		const auto exponent = (value >> 10u) & 31u;
		const auto fraction = value & 1023u;
		return exponent == 0 ? std::ldexp(double(fraction), -24)
		                     : std::ldexp(1.0 + double(fraction) / 1024.0,
		                                  int(exponent) - 15);
	};
	double total = 0.0;
	double peak = 0.0;
	uint32_t visible = 0;
	for (uint32_t i = 0; i < count; ++i) {
		std::array<uint16_t, 4> rgba {};
		std::memcpy(rgba.data(), mapped + i * texel_bytes, texel_bytes);
		const auto lum = 0.2126 * half(rgba[0]) + 0.7152 * half(rgba[1]) +
		                 0.0722 * half(rgba[2]);
		total += std::min(lum, 4.0);
		peak = std::max(peak, lum);
		visible += lum >= 0.006;
	}
	const auto level = total / count;
	const auto state = DemonsAdaptiveCapture::Current().Observe(
	    frame, level, visible, selected->desc.info.data.address);
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_GAMEPLAY_LIGHT_MONITOR] frame={} scene=0x{:x} level={:.7f} "
	    "lit_pixels={}/{} peak_luminance={:.7f} bright_reference={:.7f} low_streak={} "
	    "triggered={} reason={} owner=0x{:x} version={}\n",
	    frame, selected->desc.info.data.address, level, visible, count, peak,
	    state.bright_reference, state.low_streak, state.triggered, state.reason,
	    image.info.data.address, image.ContentVersion()));
}

// The older post-pass probe followed the active light tiles, so its source
// coordinates changed with the camera. A black sample in frame B cannot be
// compared to a nonblack sample in frame A unless both use the same XY.
// Sample the post-processing texture at fixed normalized coordinates once per
// selected frame, preserving its native view/mip and ownership metadata.
static void ProbeDsPostFixedInput(RenderContext& context, const PreparedBindings& pixel,
                                  uint64_t shader, int32_t frame) {
	if (shader != 0xd1768d02fcf96a58ull && shader != 0x844e5697cf62ecd7ull) return;
	static const bool enabled = [] {
		const auto* v = std::getenv("KYTY_DS_FRAME_TRACE");
		return v != nullptr && v[0] == '1' && v[1] == '\0';
	}();
	if (!enabled || Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	static DemonsRollingProbe::FrameGate baseline;
	static DemonsRollingProbe::FrameGate event_capture;
	static uint32_t samples = 0;
	if (samples >= 20u) return;
	const bool baseline_due = baseline.Due(frame, 192);
	const bool event_due = DemonsAdaptiveCapture::Current().CaptureDue(frame) &&
	                       event_capture.Due(frame, 24);
	if (!baseline_due && !event_due) return;
	// The source image is dynamically allocated. The prior hard-coded
	// 0x346d00000 address was absent in the 718 capture (the same shader
	// sampled 0x346900000 instead), silently disabling the diagnostic.
	// Select an actual shader read binding, never a stale allocation.
	const auto& resources = pixel.runtime->program->info.images;
	const auto first = pixel.images.begin();
	const auto found = std::find_if(first, pixel.images.end(), [&](const auto& input) {
		const auto index = static_cast<size_t>(&input - pixel.images.data());
		return index < resources.size() && resources[index].read &&
		       !resources[index].written && input.image_id && !input.placeholder;
	});
	if (found == pixel.images.end()) return;
	const auto& image = context.GetTextureCache().GetImage(found->image_id);
	const auto view = std::ranges::find(image.views, found->image_view,
	                                   &CachedImageView::view);
	if (view == image.views.end()) return;
	++samples;
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_POST_FIXED_INPUT] frame={} shader=0x{:016x} guest=0x{:x} "
	    "owner=0x{:x} version={} mip={} layer={} "
	    "gpu_modified={} cpu_dirty={} trigger={} slot={}\n",
	    frame, shader, found->desc.info.data.address, image.info.data.address,
	    image.ContentVersion(), view->info.base_level, view->info.base_layer,
	    image.IsGpuModified(), image.IsCpuDirty(), event_due,
	    static_cast<size_t>(found - pixel.images.begin())));
	RenderColorInfo color {};
	color.image_id = found->image_id;
	color.desc = found->desc;
	// Null pixel coordinates use the same 5 fixed normalized positions on
	// every sample. Unlike the old tile-following comparison this is valid.
	ProbeDsColorOutputs(context, &color, 1u, shader, false, true, &view->info);
}

// Only the actual Demon's Souls scene writer and its R16G16B16A16
// inputs are sampled. The same normalized 8x8 points are used in each
// frame and each pass; tile-following samples cannot prove full-scene loss.
struct DsSceneGridSnapshot {
	bool valid = false;
	uint32_t width = 0, height = 0;
	double luminance = 0.0;
	double peak = 0.0;
	uint32_t lit = 0;
	std::array<uint16_t, 64 * 4> rgba {};
};

static DsSceneGridSnapshot SnapshotDsSceneGrid(RenderContext& context, ImageId id,
                                                const ImageViewInfo& view) {
	DsSceneGridSnapshot result {};
	if (!id) return result;
	auto& image = context.GetTextureCache().GetImage(id);
	const auto& backing = image.backing;
	if (!backing.image || backing.image_type != vk::ImageType::e2D ||
	    backing.samples != 1 || backing.format != vk::Format::eR16G16B16A16Sfloat ||
	    !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	    view.base_level >= backing.mip_levels || view.base_layer >= backing.layers) return result;
	const auto width = std::max(backing.extent.width >> view.base_level, 1u);
	const auto height = std::max(backing.extent.height >> view.base_level, 1u);
	if (width < 16u || height < 16u) return result;
	constexpr uint32_t count = 64u, texel_bytes = 8u;
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(count * texel_bytes, 16u, true);
	if (!mapped) return result;
	download.Commit();
	std::array<vk::BufferImageCopy, count> copies {};
	for (uint32_t i = 0; i < count; ++i) {
		auto& copy = copies[i];
		copy.bufferOffset = offset + uint64_t{i} * texel_bytes;
		copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, view.base_level, view.base_layer, 1};
		copy.imageOffset = {static_cast<int32_t>((i % 8u * 2u + 1u) * width / 16u),
		                    static_cast<int32_t>((i / 8u * 2u + 1u) * height / 16u), 0};
		copy.imageExtent = {1u, 1u, 1u};
	}
	image.Download(std::span{copies}, download.Handle(), offset, count * texel_bytes);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, count * texel_bytes);
	std::memcpy(result.rgba.data(), mapped, result.rgba.size() * sizeof(uint16_t));
	result.valid = true;
	result.width = width;
	result.height = height;
	const auto half = [](uint16_t value) -> double {
		if (value & 0x8000u || (value & 0x7c00u) == 0x7c00u) return 0.0;
		const auto exponent = (value >> 10u) & 31u;
		const auto fraction = value & 1023u;
		return exponent == 0 ? std::ldexp(double(fraction), -24)
		                     : std::ldexp(1.0 + double(fraction) / 1024.0, int(exponent) - 15);
	};
	for (uint32_t i = 0; i < count; ++i) {
		const auto r = half(result.rgba[4u * i]);
		const auto g = half(result.rgba[4u * i + 1u]);
		const auto b = half(result.rgba[4u * i + 2u]);
		const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		result.luminance += std::min(lum, 4.0);
		result.peak = std::max(result.peak, lum);
		result.lit += lum >= 0.006;
	}
	result.luminance /= count;
	return result;
}

static nlohmann::json DsSceneGridJson(const DsSceneGridSnapshot& value) {
	return {{"valid", value.valid}, {"width", value.width}, {"height", value.height},
	        {"level", value.luminance}, {"peak", value.peak}, {"lit_pixels", value.lit},
	        {"rgba16_bits", value.valid ? nlohmann::json(value.rgba) : nlohmann::json()}};
}

uint32_t PickDsColoredTilePixels(RenderContext& context, ImageId id,
                                const ImageViewInfo& view, std::span<const uint32_t> tiles,
                                uint32_t width, uint32_t height, DsProbePixels& pixels) {
	if (!id || tiles.empty()) return 0;
	RenderColorInfo color {};
	color.image_id = id;
	color.desc.view_info = view;
	const auto snapshot = SnapshotDsAlbedo(context, color);
	if (snapshot.width != width || snapshot.height != height) return 0;
	return DemonsQueueProbe::ColoredTilePixels(snapshot.words, tiles, width, height, pixels);
}

// The compute renderer shares the same exact-pixel sampler rather than
// inferring nonzero outputs from vkCmdDispatch counts.
void ProbeDsComputeImage(RenderContext& context, ImageId id, uint64_t shader_hash,
                         bool forced, const ImageViewInfo* view, const DsProbePixels* pixels,
                         std::array<uint32_t, DsProbePixels{}.size()>* first_words) {
	if (!id) return;
	RenderColorInfo color {};
	color.image_id = id;
	ProbeDsColorOutputs(context, &color, 1u, shader_hash, true, forced, view, pixels, 0, 0, first_words);
}

DemonsQueueProbe::Capture* RenderExecutor::DsPostCapture() {
	using namespace DemonsQueueProbe;
	const auto frame = m_context.GetGpu().GetFrameNum();
	auto& capture = m_ds_queue_captures.back();
	// The second measured queue completes after the primary light passes.
	// Keep a separate slot for the final composite even if many passes match.
	if (!capture.light_output_sampled || capture.composite_sampled ||
	    !PostTracePending(capture.frame, frame)) return nullptr;
	return &capture;
}

void RenderExecutor::ProbeDsPostImages(const PreparedBindings& bindings,
                                       const DemonsQueueProbe::Capture& capture,
                                       uint64_t submit, uint64_t shader, bool compute, bool before,
                                       bool all_inputs) {
	using namespace DemonsQueueProbe;
	for (uint32_t i = 0; i < bindings.images.size(); ++i) {
		const auto& binding = bindings.images[i];
		const auto address = binding.desc.info.data.address;
		const bool written = i < bindings.runtime->program->info.images.size() &&
		    bindings.runtime->program->info.images[i].written;
		if ((!capture.TracesImage(address) && !(all_inputs && before && !written)) ||
		    (!before && !written)) continue;
		if (binding.placeholder || !binding.image_id) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_POST_BINDING] light_frame={} pass={} submit={} shader=0x{:016x} phase={} "
			    "guest=0x{:x} slot={} missing=true\n",
			    capture.frame, capture.post_passes, submit, shader, before ? "before" : "after",
			    address, i));
			continue;
		}
		const auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
		const auto view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
		const auto version = image.ContentVersion();
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_POST_BINDING] light_frame={} pass={} submit={} shader=0x{:016x} phase={} "
		    "guest=0x{:x} slot={} owner=0x{:x} version={} written={} view={}\n",
		    capture.frame, capture.post_passes, submit, shader, before ? "before" : "after",
		    address, i, image.info.data.address, version, written, view != image.views.end()));
		if (view == image.views.end()) continue;
		RenderColorInfo source {};
		source.image_id = binding.image_id;
		ProbeDsColorOutputs(m_context, &source, 1, shader, compute, true, &view->info,
		                    &capture.pixels, capture.width, capture.height);
	}
}

DemonsQueueProbe::Capture* RenderExecutor::BeginDsPostCompute(const PreparedBindings& bindings,
                                                               uint64_t submit) {
	const auto shader = bindings.runtime->program->shader_hash;
	auto* capture = DsPostCapture();
	if (!capture || shader == DemonsQueueProbe::Shaders.back() ||
	    std::ranges::none_of(bindings.images, [capture](const auto& binding) {
		    return capture->TracesImage(binding.desc.info.data.address);
	    })) return nullptr;
	const auto& resources = bindings.runtime->program->info.images;
	bool writes_scene = false;
	bool reads_tracked = false;
	for (size_t i = 0; i < bindings.images.size() && i < resources.size(); ++i) {
		const auto& binding = bindings.images[i];
		writes_scene |= resources[i].written &&
		    binding.desc.info.data.address == DemonsQueueProbe::SceneAddress;
		reads_tracked |= !resources[i].written && capture->TracesImage(binding.desc.info.data.address);
	}
	if (!DemonsQueueProbe::TracePostPass(capture->post_passes, capture->scene_passes,
	                                    writes_scene, false)) return nullptr;
	capture->scene_passes += writes_scene;
	if (reads_tracked) {
		for (size_t i = 0; i < bindings.images.size() && i < resources.size(); ++i) {
			const auto& binding = bindings.images[i];
			if (!resources[i].written || !binding.image_id || binding.placeholder) continue;
			const auto& extent = binding.desc.info.extent;
			capture->TrackIntermediate(binding.desc.info.data.address, extent.width, extent.height);
		}
	}
	++capture->post_passes;
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_POST_PASS] light_frame={} frame={} pass={} submit={} shader=0x{:016x} "
	    "path=compute writes_scene={}\n",
	    capture->frame, m_context.GetGpu().GetFrameNum(), capture->post_passes, submit, shader,
	    writes_scene));
	ProbeDsPostImages(bindings, *capture, submit, shader, true, true, writes_scene);
	return capture;
}

static uint32_t DrawColorOutputMask(const HW::Context& ctx) {
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto  write_mask  = ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    output_mask = ctx.HasObjectPrimitiveIdOutput() ? 1u << 7u : 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh_regs.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh_regs.target_output_mode[index] != 0 &&
		    render_target_mask_slot(write_mask, slot) != 0) {
			output_mask |= 1u << slot;
		}
	}
	// Chen's Demon renderer uses CB_TARGET_MASK & CB_SHADER_MASK to decide
	// whether a present PS is active and which guest color attachments it
	// prepares. The Android prefilter additionally required output_mode and
	// an inferred export-target mapping. When those were absent it marked a
	// genuinely color-writing PS inactive *before translating the shader*.
	// Recover only guest-enabled targets with real backing addresses at this
	// earlier point. The translated shader still determines what it exports:
	// unlike the old post-translation MRT experiment, this cannot invent an
	// output for a depth-only shader.
	// Match the Chen KytyPS5 PS activation rule by default for the verified
	// Demon's Souls title. Only actual CB_TARGET/CB_SHADER-mask writes with
	// real guest backings are restored; PS export locations are still determined
	// by shader translation. Keep a reversible opt-out for Android drivers.
	static const bool ds_cb_mask_prepare = [] {
		const char* v = std::getenv("KYTY_DS_CB_MASK_PREPARE");
		if (v != nullptr) return v[0] == '1' && v[1] == '\0';
		std::string title;
		return Loader::SystemContentParamSfoGetString("TITLE_ID", &title) &&
		       title == "PPSA01341";
	}();
	if (ds_cb_mask_prepare) {
		uint32_t guest_writes = 0, present_targets = 0;
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; ++slot) {
			if (render_target_mask_slot(write_mask, slot) != 0) {
				guest_writes |= 1u << slot;
			}
			if (ctx.GetRenderTarget(slot).base.addr != 0) {
				present_targets |= 1u << slot;
			}
		}
		const auto expanded = RecoverGuestColorTargets(
		    output_mask, guest_writes, present_targets);
		if (expanded != output_mask) {
			static std::atomic_uint32_t recovered {0};
			const uint32_t n = recovered.fetch_add(1u, std::memory_order_relaxed);
			if (n < 48u || (n % 8192u) == 0u) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_CB_MASK_PREPARE] recovered={} old=0x{:x} new=0x{:x} "
				    "guest=0x{:x} targets=0x{:x}\n",
				    n, output_mask, expanded, guest_writes, present_targets));
			}
		}
		output_mask = expanded;
	}
	return output_mask;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

bool RenderExecutor::ConsumeMetadataColorOperation(CommandBuffer& buffer,
                                                   uint32_t render_target_slice_offset) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	if (mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)) {
		// FCE can precede any ordinary draw. Discover the target to materialize its native
		// clear metadata before the expanded image is sampled.
		RenderColorInfo target {};
		ResolveRenderColorTarget(buffer, target, render_target_slice_offset, 0, true);
		return true;
	}
	// Metadata/decompression shaders must not export colors into the expanded image.
	return mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	std::array<vk::DeviceSize, MaxBuffers> sizes {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes {};
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw,
                           uint32_t color_output_mask, DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	if (auto* prepared = GpuPipeline::t_prepared_draw; prepared != nullptr) {
		// KYTY_GPU_PIPELINE: the front walked this draw's SRT from the same registers
		// (RenderExecutor::PrefetchDraw).
		EXIT_IF(prepared->color_output_mask != color_output_mask ||
		        prepared->ps_active != state.ps_active);
		state.programs      = prepared->programs;
		state.vertex_info   = prepared->vertex_info;
		state.ps_input_info = prepared->ps_input_info;
		state.indirect      = prepared->indirect;
		return;
	}
	// The record thread walks only under KYTY_GPU_FAN (it owns the program cache then).
	EXIT_IF(GpuPipeline::OnBack() && WalkFan::t_prewalk == nullptr);
	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			target_export_mapping[slot] =
			    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                 rt.info.channel_order)
			        .export_mapping;
		}
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "GetGraphicsPrograms");
	}
	PipelineProbe::BeginItem();
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vertex_info, state.ps_input_info,
	    state.indirect);
	if (state.programs.ready && Common::PerfStats::Enabled()) {
		bool   dma   = false;
		size_t bytes = 0;
		const auto note = [&](const ShaderStageRuntime& stage) {
			if (stage.program != nullptr) dma = dma || stage.program->has_address_writes;
			bytes += PipelineProbe::ResourceBytes(stage.resources);
		};
		for (uint32_t i = 0; i < state.programs.VertexStageCount(); i++) note(state.vertex_info[i].stage);
		if (state.ps_active) note(state.ps_input_info.stage);
		if (dma) PipelineProbe::MarkDmaWriter();
		PipelineProbe::AddResourceBytes(bytes);
	}
}

// KYTY_GPU_PIPELINE front: the SRT walk of a DrawIndex / DrawAuto from the live registers, or
// why the draw cannot be queued with one. Mirrors the draw's early returns before
// PrepareDrawRenderState; metadata, resolve and depth-copy draws touch the texture cache before
// they would walk, so they run on the front after a drain.
GpuPipeline::Prefetch RenderExecutor::PrefetchDraw(CommandBuffer& buffer, bool indexed,
                                                   uint32_t count, uint32_t instance_count,
                                                   GpuPipeline::PreparedDrawShaders& out) {
	using GpuPipeline::Prefetch;
	if (count == 0 || instance_count == 0) {
		return Prefetch::NoWalk;
	}
	const auto& hw   = buffer.GetRegisters();
	const auto  mode = hw.GetColorControl().mode;
	if (mode != static_cast<uint8_t>(CbColorMode::Disable) &&
	    mode != static_cast<uint8_t>(CbColorMode::Normal)) {
		return Prefetch::Inline; // fast-clear elimination, resolve, FMASK/DCC decompression
	}
	const auto& override = hw.GetDepthRenderOverride();
	if (mode == static_cast<uint8_t>(CbColorMode::Disable) &&
	    ((override.force_z_dirty && override.force_z_valid) ||
	     (override.force_stencil_dirty && override.force_stencil_valid))) {
		return Prefetch::Inline; // a possible DepthStencilCopy
	}
	if (!DrawHasValidVertexShader(buffer.GetShaders())) {
		return Prefetch::NoWalk;
	}
	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(buffer.GetUserConfig(), topology)) {
		return Prefetch::NoWalk;
	}
	const DrawCallInfo draw {indexed ? CommandBufferDebugOp::DrawIndex
	                                 : CommandBufferDebugOp::DrawIndexAuto,
	                         count, instance_count, 0};
	const auto& shader_regs  = hw.GetShaderRegisters();
	out.color_output_mask    = DrawColorOutputMask(hw);
	DrawRenderState state {};
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (out.color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, out.color_output_mask, state);
	out.ps_active     = state.ps_active;
	out.programs      = state.programs;
	out.vertex_info   = state.vertex_info;
	out.ps_input_info = state.ps_input_info;
	out.indirect      = state.indirect;
	if (out.programs.ready) {
		out.OwnResources();
	}
	return Prefetch::Prepared;
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
	                                        DrawRenderState& state) {
	const auto& shader_regs       = buffer.GetRegisters().GetShaderRegisters();
	const auto  color_output_mask = DrawColorOutputMask(buffer.GetRegisters());
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, color_output_mask, state);
	if (state.programs.ready)
		state.previous_vertex_stages = state.programs.VertexStageCount();
	if (!state.programs.ready) {
		TraceDsGraphicsOutcome(false, false, 0, false, state.ps_active);
		// --async-shaders: a program is still translating on a worker. The draw is dropped for
		// this frame (its stage pointers are unset) and drawn once the program is installed.
		Common::PerfStats::CountSkippedDraw();
		return false;
	}
	uint32_t mrt_mask = 0;
	if (state.ps_active) {
		for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt ||
			    output.kind == ShaderRecompiler::IR::StageOutputKind::ObjectPrimitiveId) {
				mrt_mask |= 1u << output.location;
			}
		}
	}
	mrt_mask &= color_output_mask;
	// Match the reference PS5 renderer: attachment lifetime follows guest
	// CB_TARGET_MASK, not the subset of PS exports the translator detects.
	// Static/dynamic write masks still honor CB_SHADER_MASK. Even depth-only
	// passes may have a live color attachment that must stay attached.
	static const bool ds_guest_color_targets = [] {
		const char* setting = std::getenv("KYTY_DS_GUEST_COLOR_TARGETS");
		return setting && setting[0] == '1' && setting[1] == '\0';
	}();
	if (ds_guest_color_targets) {
		const auto& ctx = buffer.GetRegisters();
		uint32_t guest_mask = 0;
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; ++slot) {
			const auto& rt = ctx.GetRenderTarget(slot);
			if (rt.base.addr != 0 &&
			    rt.info.format != Prospero::ChannelLayout::kInvalid &&
			    render_target_mask_slot(ctx.GetRenderTargetMask(), slot) != 0)
				guest_mask |= 1u << slot;
		}
		mrt_mask = guest_mask;
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((mrt_mask & (1u << slot)) != 0) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(buffer, state.depth_info);
	// TODO(PPSA09243): Vulkan passes take one sample count. Where PS5 attachments mix counts,
	// keep those matching the raster count instead of resolving multisampled shadows.
	const auto colors       = std::span {state.color_info, state.color_count};
	const auto pass_samples = state.depth_info.image_id ? state.depth_info.desc.info.samples
	                                                    : state.color_info[0].desc.info.samples;
	if (std::ranges::any_of(colors, [&](const RenderColorInfo& color) {
		    return color.desc.info.samples != pass_samples;
	    })) {
		const auto raster =
		    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
		const auto dropped = std::ranges::remove_if(colors, [&](const RenderColorInfo& color) {
			return color.desc.info.samples != raster;
		});
		state.color_count -= static_cast<uint32_t>(dropped.size());
		if (state.depth_info.image_id && state.depth_info.desc.info.samples != raster) {
			state.depth_info = {};
		}
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(
			    "Warning: dropping attachments whose sample count differs from the raster.\n");
		}
	}

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.IsIndexed() && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

// KYTY_LIVE_KNOBS skip=: a vertex-stage or pixel shader hash matches.
static bool SkipDrawForLiveKnobs(const DrawRenderState&                state,
                                 std::span<const ShaderVertexInputInfo> vertex_stages) {
	for (const auto& stage: vertex_stages) {
		if (stage.stage.program != nullptr && LiveKnobs::SkipShader(stage.stage.program->shader_hash)) {
			return true;
		}
	}
	return state.ps_active && state.ps_input_info.stage.program != nullptr &&
	       LiveKnobs::SkipShader(state.ps_input_info.stage.program->shader_hash);
}

// KYTY_LIVE_KNOBS marks=1: times one draw, labelled by its emulation path and pixel shader (the
// vertex shader when there is none), with the first vertex-stage shader as the second hash.
static void MarkDrawForLiveKnobs(const CommandBuffer& buffer, const DrawRenderState& state,
                                 std::span<const ShaderVertexInputInfo> vertex_stages,
                                 const RenderState& rendering, bool mesh_emulated,
                                 uint64_t indices) {
	const auto* ps = state.ps_active ? state.ps_input_info.stage.program : nullptr;
	const auto* vs = vertex_stages.empty() ? nullptr : vertex_stages.front().stage.program;
	const bool  gs = ps != nullptr && ps->info.barycentric_emulation.enabled;
	const bool  rect = Prospero::IsRectList(buffer.GetUserConfig().GetPrimType());
	const char* kind = mesh_emulated            ? (gs ? "draw-mesh-gs" : "draw-mesh")
	                   : vertex_stages.size() > 1 ? (gs ? "draw-tess-gs" : "draw-tess")
	                   : rect                    ? (gs ? "draw-rect-gs" : "draw-rect")
	                   : gs                      ? "draw-gs"
	                                             : "draw";
	const uint64_t named = ps != nullptr ? ps->shader_hash : (vs != nullptr ? vs->shader_hash : 0);
	buffer.MarkGpu(kind, named,
	               {.width   = rendering.width,
	                .height  = rendering.height,
	                .colors  = rendering.num_color_attachments,
	                .depth   = rendering.depth_stencil_attachment.image_view != nullptr,
	                .draws   = 1,
	                .indices = indices,
	                .aux     = ps != nullptr && vs != nullptr ? vs->shader_hash : 0});
}

// --perf-stats: every 16th GPU-executed indirect draw (at most 256 lines) names its shaders and
// the first guest record as the backing holds it (stale when the GPU wrote it: "gpu_dirty=1").
static void LogIndirectDrawSample(const DrawRenderState& state, const IndirectDrawArgs& args) {
	static std::atomic<uint32_t> seen {0};
	const auto                   n = seen.fetch_add(1, std::memory_order_relaxed);
	if (n % 16u != 0u || n / 16u >= 256u) {
		return;
	}
	const auto* ps = state.ps_active ? state.ps_input_info.stage.program : nullptr;
	const auto* vs = state.vertex_info[0].stage.program;
	std::array<uint32_t, 5> record {};
	const bool read = Libs::LibKernel::Memory::TryReadBacking(
	    args.args_addr, record.data(), (args.indexed ? 5u : 4u) * sizeof(uint32_t));
	char line[320];
	std::snprintf(line, sizeof(line),
	              "[perf-indirect] ps=0x%016" PRIx64 " vs=0x%016" PRIx64 " indexed=%d max=%u "
	              "count_addr=%d args=0x%010" PRIx64 " backing=%s %u %u %u %u %u",
	              ps != nullptr ? ps->shader_hash : 0, vs != nullptr ? vs->shader_hash : 0,
	              args.indexed ? 1 : 0, args.max_count, args.count_addr != 0 ? 1 : 0,
	              args.args_addr, read ? "ok" : "no", record[0], record[1], record[2], record[3],
	              record[4]);
	Common::PerfStats::EmitLine(line);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

bool RenderExecutor::CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
                                       const DrawEmitInfo& emit, DrawRenderState& state) {
	if (!buffer.GetRegisters().GetRenderControl().copy_depth_to_color) {
		return false;
	}
	auto& ucfg = buffer.GetUserConfig();
	const auto& hw = buffer.GetRegisters();
	auto& depth = state.depth_info;
	auto& color = state.color_info[0];
	EXIT_NOT_IMPLEMENTED(!m_context.GetGraphics().maintenance8_enabled ||
	                     !Prospero::IsRectList(ucfg.GetPrimType()) ||
	                     draw.IsIndexed() || draw.index_count != 3 || draw.instance_count != 1 ||
	                     emit.first_vertex != 0 || emit.first_instance != 0 ||
	                     state.color_count != 1 || color.target_slot != 0 || !depth.image_id ||
	                     color.desc.info.samples != 1 ||
	                     depth.desc.info.extent.width != color.Extent().width ||
	                     depth.desc.info.extent.height != color.Extent().height ||
	                     depth.desc.view_info.layer_count != 1 ||
	                     color.desc.view_info.layer_count != 1 ||
	                     hw.GetBlendControl(0).enable || state.ps_input_info.ps_pixel_kill_enable ||
	                     state.ps_input_info.ps_depth_export_enable ||
	                     state.ps_input_info.ps_sample_mask_export_enable);
	vk::ImageAspectFlags feedback;
	const auto rendering = AcquireRenderTargets(buffer, &color, 1, depth, feedback);
	if (depth.depth_load_clear_enable) {
		m_context.GetCommandScheduler().BeginRendering(rendering);
	}
	auto& cache = m_context.GetTextureCache();
	auto& source = cache.GetImage(depth.image_id);
	auto& destination = cache.GetImage(color.image_id);
	EXIT_NOT_IMPLEMENTED((source.backing.format != vk::Format::eD32Sfloat &&
	                      source.backing.format != vk::Format::eD32SfloatS8Uint) ||
	                     destination.backing.format != vk::Format::eR32Sfloat);
	const auto rect = HostScissor(hw, rendering, 0);
	if (rect.right > rect.left && rect.bottom > rect.top) {
		const vk::ImageCopy region {
		    {vk::ImageAspectFlagBits::eDepth, depth.desc.view_info.base_level,
		     depth.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {vk::ImageAspectFlagBits::eColor, color.desc.view_info.base_level,
		     color.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {static_cast<uint32_t>(rect.right - rect.left),
		     static_cast<uint32_t>(rect.bottom - rect.top), 1}};
		m_context.GetCommandScheduler().EndRendering(PassEnd::ImageCopy);
		const auto command = buffer.Handle();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		               {}, command);
		destination.Transit(vk::ImageLayout::eTransferDstOptimal,
		                    vk::AccessFlagBits2::eTransferWrite, {}, command);
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                  1, &region);
	}
	return true;
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable,
	                                     const IndirectDrawArgs* indirect) {
	const PipelineProbe::ScopedRecordPhase record_phase;
	auto& ucfg = buffer.GetUserConfig();
	if (CopyDepthToColor(buffer, draw, emit, state)) {
		return;
	}
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	// Without VK_EXT_mesh_shader the mesh stage runs as compute and a generated vertex stage
	// draws its output as an indexed triangle list.
	const bool mesh_emulated =
	    mesh_active && state.vertex_info[0].stage.program->info.mesh_emulation;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		// --gpu-indirect: the fix-up pass sizes the dispatch (DispatchEmulatedMeshIndirect).
		const auto primitives = indirect != nullptr ? 1u : mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (indirect != nullptr) {
			EXIT_IF(!mesh_emulated || mesh.fast_launch || !draw.IsIndexed());
		} else if (mesh_emulated) {
			const auto& compute = m_context.GetGraphics().GetPhysicalDeviceProperties().limits;
			if (mesh_groups > compute.maxComputeWorkGroupCount[0] ||
			    draw.instance_count > compute.maxComputeWorkGroupCount[1]) {
				EXIT("mesh emulation: draw exceeds host compute workgroup counts: %ux%u\n",
				     mesh_groups, draw.instance_count);
			}
		} else if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		// (--gpu-indirect: the whole INDEX_BUFFER_SIZE range.)
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, indirect != nullptr
		                              ? index_source.size
		                              : static_cast<uint64_t>(draw.index_count) *
		                                    index_source.guest_element_size);
	}
	if (indirect != nullptr) {
		// Registered before the bindings so that acquiring them below merges no bound buffer.
		auto& cache = m_context.GetBufferCache();
		(void)cache.FindBuffer(indirect->args_addr, IndirectDrawFixup::GuestRecordsSize(*indirect));
		if (indirect->count_addr != 0) {
			(void)cache.FindBuffer(indirect->count_addr, sizeof(uint32_t));
		}
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	IndirectDrawFixup::Sources indirect_sources;
	if (indirect != nullptr) {
		indirect_sources = GetIndirectDrawFixup().AcquireSources(*indirect);
	}
	if (RebindAfterJoinEnabled()) {
		auto& cache = m_context.GetBufferCache();
		for (uint32_t pass = 0; pass < 4 && cache.UnregisterEpoch() != m_rebind_epoch; ++pass) {
			NoteRebindAfterJoin("draw", draw.Name());
			m_rebind_epoch = cache.UnregisterEpoch();
			for (auto* stage: stages) RebindBuffers(*stage);
			if (!mesh_active) {
				vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
				index_binding = PrepareIndexBuffer(buffer, index_source);
			}
			if (indirect != nullptr) indirect_sources = GetIndirectDrawFixup().AcquireSources(*indirect);
		}
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	auto* pipeline_ptr = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr,
	    mesh_emulated ? vk::PrimitiveTopology::eTriangleList : topology,
	    mesh_emulated ? false : primitive_restart_enable, state.programs);
	if (pipeline_ptr != nullptr && mesh_emulated &&
	    m_context.GetPipelineCache().TryGetMeshEmulationPipeline(
	        state.vertex_info[0], state.programs.vertex[0]) == nullptr) {
		pipeline_ptr = nullptr;
	}
	if (pipeline_ptr == nullptr) {
		TraceDsGraphicsOutcome(true, false, state.color_count,
		                      static_cast<bool>(state.depth_info.image_id), state.ps_active);
		// --async-shaders: the pipeline is being created on a worker. Nothing recorded so far
		// depends on it (bindings and buffers were only prepared), so the draw is dropped for
		// this frame and drawn once the pipeline is installed.
		Common::PerfStats::CountSkippedDraw();
		return;
	}
	if (SkipDrawForLiveKnobs(state, vertex_stages)) {
		// KYTY_LIVE_KNOBS skip=: dropped at the same point as a pipeline that is not ready.
		return;
	}
	auto& pipeline = *pipeline_ptr;
	TraceDsGraphicsOutcome(true, true, state.color_count,
	                      static_cast<bool>(state.depth_info.image_id), state.ps_active);
	static const bool ds_gbuffer_enabled = [] {
		const auto* value = std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");
		return value && value[0] == '1' && value[1] == '\0';
	}();
	// Record the actual material pass feeding the deferred-lighting kernels.
	// One draw per selected shader and 240 frames keeps GPU readback bounded.
	const RenderColorInfo* ds_gbuffer = nullptr;
	uint64_t ds_gbuffer_shader = 0;
	const int32_t ds_gbuffer_frame = m_context.GetGpu().GetFrameNum();
	DemonsQueueProbe::MaterialTrace* ds_material_trace = nullptr;
	DsAlbedoSnapshot ds_albedo_before;
	if (ds_gbuffer_enabled && state.ps_active && bindings.pixel) {
		ds_gbuffer_shader = state.ps_input_info.stage.program->shader_hash;
		const auto& watched = DemonsQueueProbe::GBufferShaders;
		const auto found = std::ranges::find(watched, ds_gbuffer_shader);
		if (found != watched.end()) {
			const auto colors = std::span {state.color_info, state.color_count};
			const auto target = std::ranges::find_if(colors, [](const auto& color) {
				return color.image_id && color.desc.info.data.address == DemonsQueueProbe::GBufferAddress &&
				       color.desc.view_info.base_layer == 0 && color.desc.view_info.base_level == 0;
			});
			static std::array<DemonsRollingProbe::FrameGate, watched.size()> gates {};
			if (target != colors.end() && gates[found - watched.begin()].Due(ds_gbuffer_frame, 240)) {
				ds_gbuffer = &*target;
				ds_material_trace = &m_ds_material_traces[found - watched.begin()];
				if (ds_material_trace->count && !ds_material_trace->lighting_sampled) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_MATERIAL_LIGHT_UNMATCHED] draw_frame={} submit={} material=0x{:016x}\n",
					    ds_material_trace->frame, ds_material_trace->submit, ds_material_trace->shader));
				}
				*ds_material_trace = {ds_gbuffer_frame, ds_gbuffer_shader};
				ds_material_trace->submit = submit_id;
				ds_material_trace->image_index = target->image_id.index;
				ds_material_trace->image_generation = target->image_id.generation;
				const auto& hw = buffer.GetRegisters();
				const auto shader_mask = hw.GetShaderRegisters().m_cbShaderMask;
				const auto hardware_mask = hw.GetRenderTargetMask();
				const bool dyn_write = m_context.GetGraphics().optional_features.color_write_enable;
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_GBUFFER_DRAW] frame={} phase=before shader=0x{:016x} dynamic_write={} "
				    "target_mask=0x{:08x} shader_mask=0x{:08x} index_count={} instances={} indirect={} "
				    "depth=0x{:x} depth_test={} depth_write={} depth_func={} "
				    "stencil_test={} stencil_func={} stencil_ref={} stencil_cmask=0x{:x} stencil_wmask=0x{:x}\n",
				    ds_gbuffer_frame, ds_gbuffer_shader, dyn_write, hardware_mask, shader_mask,
				    draw.index_count, draw.instance_count, indirect != nullptr,
				    state.depth_info.desc.info.data.address,
				    state.depth_info.depth_test_enable ? 1u : 0u,
				    state.depth_info.depth_write_enable ? 1u : 0u,
				    static_cast<uint32_t>(state.depth_info.depth_compare_op),
				    state.depth_info.stencil_test_enable ? 1u : 0u,
				    static_cast<uint32_t>(state.depth_info.stencil_front.compareOp),
				    state.depth_info.stencil_front.reference,
				    state.depth_info.stencil_front.compareMask,
				    state.depth_info.stencil_front.writeMask));
				for (const auto& color : colors) {
					const auto slot = color.target_slot;
					const auto mask = GuestColorWriteMask(hardware_mask, shader_mask, slot);
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_GBUFFER_ATTACHMENT] frame={} shader=0x{:016x} slot={} guest=0x{:x} "
					    "mip={} layer={} format={} native_mask=0x{:x} old_dynamic_mask=0x{:x}\n",
					    ds_gbuffer_frame, ds_gbuffer_shader, slot, color.desc.info.data.address,
					    color.desc.view_info.base_level, color.desc.view_info.base_layer,
					    static_cast<uint32_t>(color.desc.view_info.format),
					    color.export_mapping.ApplyMask(mask & 0xfu),
					    color.export_mapping.ApplyMask(render_target_mask_slot(hardware_mask, slot))));
				}
				uint32_t material_probes = 0;
				for (size_t i = 0; i < bindings.pixel->images.size(); ++i) {
					const auto& input = bindings.pixel->images[i];
					const auto& view = input.desc.view_info;
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_GBUFFER_INPUT] frame={} shader=0x{:016x} index={} guest=0x{:x} "
					    "format={} base_mip={} base_layer={} layers={} placeholder={}\n",
					    ds_gbuffer_frame, ds_gbuffer_shader, i, input.desc.info.data.address,
					    static_cast<uint32_t>(view.format), view.base_level, view.base_layer,
					    view.layer_count, input.placeholder || !input.image_id));
					if (material_probes >= 3 || input.placeholder || !input.image_id ||
					    !DsBlockCompressed(view.format)) continue;
					const auto& image = m_context.GetTextureCache().GetImage(input.image_id);
					const auto native = std::ranges::find(image.views, input.image_view,
					                                      &CachedImageView::view);
					if (native == image.views.end()) continue;
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_MATERIAL_BINDING] frame={} shader=0x{:016x} index={} guest=0x{:x} "
					    "owner=0x{:x} mip={} layer={} cpu_dirty={} buffer_modified={}\n",
					    ds_gbuffer_frame, ds_gbuffer_shader, i, input.desc.info.data.address,
					    image.info.data.address, native->info.base_level, native->info.base_layer,
					    image.IsCpuDirty(), image.IsBufferModified()));
					RenderColorInfo source {};
					source.image_id = input.image_id;
					ProbeDsColorOutputs(m_context, &source, 1, ds_gbuffer_shader,
					                    false, true, &native->info);
					++material_probes;
				}
				ds_albedo_before = SnapshotDsAlbedo(m_context, *ds_gbuffer);
			}
		}
	}
	// Follow the first post-compute HDR writer and reader, then the compositor.
	constexpr uint8_t DsHdrReader = 1, DsComposite = 2, DsHdrWriter = 4;
	std::array<uint8_t, DemonsQueueProbe::Queues.size()> ds_draw_stage {};
	bool ds_live_composite_output = false;
	bool ds_scene_trace = false;
	uint32_t ds_scene_pass = 0;
	RenderColorInfo ds_scene_target {};
	DsSceneGridSnapshot ds_scene_before {};
	nlohmann::json ds_scene_inputs = nlohmann::json::array();
	if (state.ps_active && bindings.pixel && state.ps_input_info.stage.program && state.color_count) {
		const auto frame = m_context.GetGpu().GetFrameNum();
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		const bool composite = shader == 0x3265b01ed00a5c01ull;
		ProbeDsPostFixedInput(m_context, *bindings.pixel, shader, frame);
		const auto& inputs = bindings.pixel->images;
		// Target the scene-writing draws only, on the first verified
		// bright reference frame and on successfully paired dark frames.
		static int32_t scene_frame = -1;
		static uint32_t scene_passes = 0;
		static int32_t bright_baseline_frame = -1;
		if (scene_frame != frame) { scene_frame = frame; scene_passes = 0; }
		const auto& gate = DemonsAdaptiveCapture::Current();
		const auto colors = std::span {state.color_info, state.color_count};
		const auto scene = std::ranges::find_if(colors, [](const auto& color) {
			return color.image_id && color.desc.info.data.address == DemonsQueueProbe::SceneAddress;
		});
		if (bright_baseline_frame < 0 && scene != colors.end() && gate.BrightBaselineDue(frame))
			bright_baseline_frame = frame;
		// These are the three confirmed scene-target shaders in the 719
		// trace. Other fullscreen/geometry passes are outside this probe.
		const bool known_scene_writer =
		    shader == 0xd1768d02fcf96a58ull ||
		    shader == 0xd9a54cb7b549b1b9ull ||
		    shader == 0x844e5697cf62ecd7ull;
		if (scene != colors.end() && known_scene_writer && scene_passes < 12u &&
		    (frame == bright_baseline_frame || gate.CapturedFrame(frame)) &&
		    Config::GetShaderDumpFolder().filename() == "ds-lighting-dumps") {
			ds_scene_trace = true;
			ds_scene_pass = ++scene_passes;
			ds_scene_target = *scene;
			ds_scene_before = SnapshotDsSceneGrid(
			    m_context, ds_scene_target.image_id, ds_scene_target.desc.view_info);
			const auto& shader_images = state.ps_input_info.stage.program->info.images;
			for (size_t i = 0; i < inputs.size(); ++i) {
				const auto& input = inputs[i];
				nlohmann::json entry {
				    {"slot", i}, {"guest", input.desc.info.data.address},
				    {"bound", static_cast<bool>(input.image_id)}, {"placeholder", input.placeholder},
				    {"read", i < shader_images.size() && shader_images[i].read},
				    {"written", i < shader_images.size() && shader_images[i].written}
				};
				if (input.image_id && !input.placeholder) {
					const auto& image = m_context.GetTextureCache().GetImage(input.image_id);
					const auto view = std::ranges::find(image.views, input.image_view,
					                                   &CachedImageView::view);
					entry["owner"] = image.info.data.address;
					entry["version"] = image.ContentVersion();
					entry["format"] = static_cast<uint32_t>(image.backing.format);
					if (view != image.views.end()) {
						entry["mip"] = view->info.base_level;
						entry["layer"] = view->info.base_layer;
						if (i < shader_images.size() && shader_images[i].read &&
						    !shader_images[i].written)
							entry["grid"] = DsSceneGridJson(SnapshotDsSceneGrid(
							    m_context, input.image_id, view->info));
					}
				}
				ds_scene_inputs.push_back(std::move(entry));
			}
		}
		if (composite) MonitorDsCompositeLight(m_context, *bindings.pixel, frame);
		if (composite && DemonsAdaptiveCapture::Current().CapturedFrame(frame)) {
			// A completed coefficient->atlas pair belongs to this exact
			// frame. Sample the compositing inputs at fixed normalized
			// coordinates, then the final attachment after the draw.
			static int32_t last_paired_frame = -1;
			if (last_paired_frame != frame) {
				last_paired_frame = frame;
				ds_live_composite_output = true;
				for (const auto& input : inputs) {
					const auto addr = input.desc.info.data.address;
					if (addr != DemonsQueueProbe::SceneAddress &&
					    addr != DemonsQueueProbe::HdrAddress &&
					    addr != DemonsQueueProbe::CompositeAuxAddress) continue;
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_PAIRED_COMPOSITOR_INPUT] frame={} shader=0x{:016x} "
					    "guest=0x{:x} bound={} placeholder={}\n",
					    frame, shader, addr, static_cast<bool>(input.image_id),
					    input.placeholder));
					if (!input.image_id || input.placeholder) continue;
					const auto& image = m_context.GetTextureCache().GetImage(input.image_id);
					const auto view = std::ranges::find(image.views, input.image_view,
					                                   &CachedImageView::view);
					if (view == image.views.end()) continue;
					RenderColorInfo source {};
					source.image_id = input.image_id;
					source.desc = input.desc;
					ProbeDsColorOutputs(m_context, &source, 1, shader, false, true,
					                    &view->info);
				}
			}
		}
		const auto colors = std::span {state.color_info, state.color_count};
		const auto hdr_target = std::ranges::find_if(colors, [](const auto& color) {
			return color.image_id && color.desc.info.data.address == DemonsQueueProbe::HdrAddress;
		});
		for (size_t i = 0; i < m_ds_queue_captures.size(); ++i) {
			const auto& capture = m_ds_queue_captures[i];
			if (capture.frame != frame) continue;
			const auto hdr = std::ranges::find_if(inputs, [](const auto& input) {
				return input.desc.info.data.address == DemonsQueueProbe::HdrAddress &&
				       input.image_id && !input.placeholder;
			});
			const bool first_hdr_reader = !capture.hdr_reader_sampled && hdr != inputs.end();
			const bool final_composite = composite && !capture.composite_sampled;
			const bool first_hdr_writer = !capture.hdr_writer_sampled && hdr_target != colors.end();
			if (!first_hdr_reader && !final_composite && !first_hdr_writer) continue;
			ds_draw_stage[i] = (first_hdr_reader ? DsHdrReader : 0u) |
			                   (final_composite ? DsComposite : 0u) |
			                   (first_hdr_writer ? DsHdrWriter : 0u);
			if (first_hdr_writer) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_QUEUE_DRAW] frame={} queue={} phase=hdr_writer_before shader=0x{:016x} guest=0x{:x}\n",
				    frame, DemonsQueueProbe::Queues[i], shader, DemonsQueueProbe::HdrAddress));
				ProbeDsColorOutputs(m_context, &*hdr_target, 1, shader, false, true, nullptr,
				                    &capture.pixels, capture.width, capture.height);
			}
			if (!final_composite && !first_hdr_reader) continue;
			for (const auto& input : inputs) {
				if (!input.image_id || input.placeholder ||
				    (!final_composite && &input != &*hdr)) continue;
				const auto& image = m_context.GetTextureCache().GetImage(input.image_id);
				const auto view = std::ranges::find(image.views, input.image_view, &CachedImageView::view);
				if (view == image.views.end()) continue;
				RenderColorInfo source {};
				source.image_id = input.image_id;
				source.desc = input.desc;
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_QUEUE_DRAW] frame={} queue={} phase={} shader=0x{:016x} guest=0x{:x}\n",
				    frame, DemonsQueueProbe::Queues[i],
				    final_composite ? "composite_input" : "hdr_reader_input",
				    shader, input.desc.info.data.address));
				ProbeDsColorOutputs(m_context, &source, 1, shader, false, true, &view->info,
				                    &capture.pixels, capture.width, capture.height);
			}
		}
	}
	DemonsQueueProbe::Capture* ds_post = nullptr;
	bool ds_post_final = false;
	if (state.ps_active && bindings.pixel && state.color_count) {
		using namespace DemonsQueueProbe;
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		const auto colors = std::span{state.color_info, state.color_count};
		ds_post_final = shader == CompositeShader;
		if (auto* capture = DsPostCapture()) {
			const auto traces_image = [capture](const auto& item) {
				return capture->TracesImage(item.desc.info.data.address);
			};
			const bool reads_tracked = std::ranges::any_of(bindings.pixel->images, traces_image);
			const bool writes_scene = std::ranges::any_of(colors, [](const auto& color) {
				return color.image_id && color.desc.info.data.address == SceneAddress;
			});
			if (TracePostPass(capture->post_passes, capture->scene_passes, writes_scene, ds_post_final) &&
			    (ds_post_final || reads_tracked || std::ranges::any_of(colors, traces_image))) {
				ds_post = capture;
				ds_post->scene_passes += writes_scene && !ds_post_final;
				if (reads_tracked && !ds_post_final) {
					for (const auto& color : colors) {
						if (!color.image_id) continue;
						const auto extent = color.Extent();
						ds_post->TrackIntermediate(color.desc.info.data.address, extent.width, extent.height);
					}
				}
				++ds_post->post_passes;
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_POST_PASS] light_frame={} frame={} pass={} submit={} shader=0x{:016x} path=graphics "
				    "targets={} target0=0x{:x} writes_scene={} final={}\n",
				    ds_post->frame, m_context.GetGpu().GetFrameNum(), ds_post->post_passes, submit_id,
				    shader, state.color_count, colors.front().desc.info.data.address, writes_scene,
				    ds_post_final));
				ProbeDsPostImages(*bindings.pixel, *ds_post, submit_id, shader, false, true,
				                  writes_scene);
				for (const auto& color : colors) {
					if (!color.image_id || !traces_image(color)) continue;
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_POST_TARGET] light_frame={} pass={} phase=before guest=0x{:x} slot={}\n",
					    ds_post->frame, ds_post->post_passes, color.desc.info.data.address,
					    color.target_slot));
					ProbeDsColorOutputs(m_context, &color, 1, shader, false, true, nullptr,
					                    &ds_post->pixels, ds_post->width, ds_post->height);
				}
			}
		}
	}
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);

	// Track the final Demon scene-composition pixel shader (identified from
	// the guest output chain), not the unrelated 0x17defa... pass that reports
	// zero image bindings. Log only bound metadata, without extra GPU work.
	// This is diagnostic and does not change shader outputs or drawing.
	static const bool ds_composite_trace = [] {
		const char* v = std::getenv("KYTY_DS_FRAME_TRACE");
		return v != nullptr && v[0] == '1' && v[1] == '\0';
	}();
	if (ds_composite_trace && state.ps_active && bindings.pixel &&
	    state.ps_input_info.stage.program != nullptr &&
	    state.ps_input_info.stage.program->shader_hash == 0x3265b01ed00a5c01ull &&
	    state.color_count != 0u) {
		const int32_t frame = m_context.GetGpu().GetFrameNum();
		static DemonsRollingProbe::FrameGate composite_metadata;
		if (composite_metadata.Due(frame, 96)) {
			const auto& ps_images = state.ps_input_info.stage.program->info.images;
			const auto& bound = bindings.pixel->images;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_COMPOSITE_INPUTS] frame={} capture=rolling target=0x{:x} resources={} bound={}\n",
			    frame, state.color_info[0].desc.info.data.address,
			    ps_images.size(), bound.size()));
			auto& texture_cache = m_context.GetTextureCache();
			for (uint32_t i = 0; i < ps_images.size() && i < bound.size(); ++i) {
				const auto& b = bound[i];
				if (!b.image_id) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_COMPOSITE_IMAGE] index={} missing=1 written={}\n",
					    i, ps_images[i].written ? 1 : 0));
					continue;
				}
				const auto& img = texture_cache.GetImage(b.image_id);
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_COMPOSITE_IMAGE] index={} guest=0x{:x} owner=0x{:x} "
				    "fmt={} extent={}x{} layer={} mip={} sampled={} storage={} "
				    "placeholder={} gpu_modified={} cpu_dirty={}\n",
				    i, b.desc.info.data.address, img.info.data.address,
				    static_cast<uint32_t>(img.backing.format),
				    img.backing.extent.width, img.backing.extent.height,
				    b.desc.view_info.base_layer, b.desc.view_info.base_level,
				    ps_images[i].read ? 1 : 0, ps_images[i].written ? 1 : 0,
				    b.placeholder ? 1 : 0, img.IsGpuModified() ? 1 : 0,
				    img.IsCpuDirty() ? 1 : 0));
			}
		}
	}
	// The 4K address is no longer stable at 1080p, and the compositor hash
	// itself may differ. Discover actual PS -> large target pairs instead of
	// relying on a previous frame's shader hash or guest allocation address.
	// Lightweight one-time metadata only; zero device submissions/readbacks.
	if (ds_composite_trace && state.ps_active &&
	    state.ps_input_info.stage.program != nullptr && state.color_count != 0u &&
	    state.color_info[0].image_id) {
		const auto& color_owner = m_context.GetTextureCache().GetImage(
		    state.color_info[0].image_id);
		if (color_owner.backing.extent.width >= 960u &&
		    color_owner.backing.extent.height >= 540u) {
			// Never stop after the first 64 pairs or assume gameplay starts
			// after an arbitrary frame. Metadata-only; no new GPU submission.
			static std::atomic_uint64_t eligible {0};
			const uint64_t ordinal = eligible.fetch_add(1u, std::memory_order_relaxed);
			if (ordinal == 0u || ordinal % 512u == 0u) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_LARGE_PS_TARGET] frame={} sample={} ps=0x{:016x} target=0x{:x} "
				    "format={} extent={}x{} images={} bound={} gpu_modified={}\n",
				    m_context.GetGpu().GetFrameNum(), ordinal,
				    state.ps_input_info.stage.program->shader_hash,
				    state.color_info[0].desc.info.data.address,
				    static_cast<uint32_t>(color_owner.backing.format),
				    color_owner.backing.extent.width, color_owner.backing.extent.height,
				    state.ps_input_info.stage.program->info.images.size(),
				    bindings.pixel ? bindings.pixel->images.size() : 0u,
				    color_owner.IsGpuModified() ? 1 : 0));
			}
		}
	}
	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);
	if (state.ps_active && !draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
	}
	const std::array<uint32_t, ShaderRecompiler::IR::PushData::MeshDrawDwordCount> mesh_draw_data {
	    draw.index_count,
	    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
	    emit.first_instance, index_source.guest_element_size,
	    static_cast<uint32_t>(index_source.address),
	    static_cast<uint32_t>(index_source.address >> 32u)};
	// Everything that may still end the open render pass (compute dispatch, image layout
	// transitions, barriers) is recorded on the primary buffer first; the pass commands then go
	// to the pass's secondary buffer and nothing in between may end the pass.
	IndirectDrawFixup::Records indirect_records;
	if (indirect != nullptr && !mesh_active) {
		indirect_records = m_indirect_fixup->Record(buffer, *indirect, indirect_sources);
	}
	if (indirect != nullptr && Common::PerfStats::Enabled()) {
		LogIndirectDrawSample(state, *indirect);
	}
	EmulatedMeshDraw emulated_mesh;
	if (mesh_emulated && indirect != nullptr) {
		emulated_mesh = DispatchEmulatedMeshIndirect(buffer, state.vertex_info[0],
		                                             state.programs.vertex[0], bindings.vertex[0],
		                                             *indirect, indirect_sources);
		PrepareBindingWrites(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages.subspan(1));
	} else if (mesh_emulated) {
		emulated_mesh = DispatchEmulatedMesh(buffer, state.vertex_info[0], state.programs.vertex[0],
		                                     bindings.vertex[0], mesh_draw_data, mesh_groups,
		                                     draw.instance_count);
		// Only the pixel stage binds resources in the graphics pipeline.
		PrepareBindingWrites(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages.subspan(1));
	} else {
		PrepareBindingWrites(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	}
	if (mesh_emulated &&
	    LiveKnobs::SkipMeshDraw(state.vertex_info[0].stage.program->shader_hash)) {
		// KYTY_LIVE_KNOBS mesh-skip-draw=: the compute stage above wrote the ring; no draw. The
		// render targets were acquired but no pass begins and they are not marked written
		// (acceptable for a diagnostic; the caller resets the bindings).
		return;
	}

	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(*stage.stage.program));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		// As for dispatches, buffer writes wait for earlier accesses to the memory (upstream
		// 70668fbd: this replaces the global barriers PM4 flush events and releases emitted).
		m_context.GetCommandScheduler().EndRendering(PassEnd::ShaderWrite);
		ShaderWriteHazardBarrier(buffer.Handle(), shader_write_stages);
	}
	// Repeat only the sampled G-buffer draw, using the same resources and
	// native raster state. The diagnostic must not modify guest attachments.
	const auto stage_image_writes = [](const auto& stage) {
		if (stage.program == nullptr) return false;
		if (stage.program->has_address_writes || stage.program->info.uses_gds) return true;
		return std::ranges::any_of(stage.program->info.images, [](const auto& image) {
			return image.written;
		});
	};
	const bool image_side_effects =
	    std::ranges::any_of(vertex_stages, [&](const auto& stage) {
		    return stage_image_writes(stage.stage);
	    }) || (state.ps_active && stage_image_writes(state.ps_input_info.stage));
	DemonsQueueProbe::CoveragePixels coverage_points {};
	constexpr uint32_t CoverageQueries = 2 + 2 * DemonsQueueProbe::CoveragePixels{}.size();
	vk::QueryPool coverage_pool {};
	const auto device = m_context.GetGraphics().device;
	const bool stencil_independent = !state.depth_info.stencil_test_enable ||
	    (state.depth_info.stencil_front.compareOp == vk::CompareOp::eAlways &&
	     state.depth_info.stencil_back.compareOp == vk::CompareOp::eAlways);
	if (ds_gbuffer && state.depth_info.image_id && stencil_independent && !indirect && !mesh_active && !shader_write_stages && !image_side_effects &&
	    state.depth_info.depth_test_enable && !state.depth_info.depth_write_enable &&
	    state.depth_info.depth_compare_op == vk::CompareOp::eEqual &&
	    m_context.GetGraphics().optional_features.color_write_enable) {
		vk::QueryPoolCreateInfo info {};
		info.queryType = vk::QueryType::eOcclusion;
		info.queryCount = CoverageQueries;
		const auto result = device.createQueryPool(&info, nullptr, &coverage_pool);
		if (result == vk::Result::eSuccess) {
			m_context.GetCommandScheduler().EndRendering(PassEnd::Buffer);
			buffer.Handle().resetQueryPool(coverage_pool, 0, CoverageQueries);
		} else {
			coverage_pool = nullptr;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_GBUFFER_COVERAGE_SKIP] frame={} shader=0x{:016x} reason={}\n",
			    ds_gbuffer_frame, ds_gbuffer_shader, vk::to_string(result)));
		}
	} else if (ds_gbuffer) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_GBUFFER_COVERAGE_SKIP] frame={} shader=0x{:016x} "
		    "indirect={} mesh={} storage_write={} depth_test={} depth_write={} depth_func={} stencil_independent={} color_enable={}\n",
		    ds_gbuffer_frame, ds_gbuffer_shader, indirect != nullptr, mesh_active,
		    static_cast<bool>(shader_write_stages) || image_side_effects,
		    state.depth_info.depth_test_enable, state.depth_info.depth_write_enable,
		    static_cast<uint32_t>(state.depth_info.depth_compare_op), stencil_independent,
		    m_context.GetGraphics().optional_features.color_write_enable));
	}
	uint32_t coverage_count = 0;
	uint32_t coverage_points_used = 0;
	LogDrawPhase(draw.Name(), "BeginRendering");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	// The emulated mesh stage ran ahead of the pass; only the remaining stages read in it.
	MarkPassImages(buffer, mesh_emulated ? stages.subspan(1) : stages,
	               std::span {state.color_info, state.color_count}, state.depth_info);
	const auto vk_buffer = buffer.PassHandle();
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (mesh_emulated) {
		RecordBindings(vk_buffer, vk::PipelineBindPoint::eGraphics, pipeline);
		vk_buffer.bindIndexBuffer(emulated_mesh.index_buffer, emulated_mesh.index_offset,
		                          vk::IndexType::eUint32);
		// MeshVertexPullData: record address, record count, out-of-range counter address.
		const uint32_t vertex_data[] {
		    static_cast<uint32_t>(emulated_mesh.vertex_address),
		    static_cast<uint32_t>(emulated_mesh.vertex_address >> 32u), emulated_mesh.vertex_count,
		    static_cast<uint32_t>(emulated_mesh.out_of_range_counter),
		    static_cast<uint32_t>(emulated_mesh.out_of_range_counter >> 32u)};
		static_assert(sizeof(vertex_data) <=
		              ShaderRecompiler::IR::PushData::MeshEmulationDrawDwordCount * sizeof(uint32_t));
		vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_constant_stages, 0,
		                        sizeof(vertex_data), vertex_data);
	} else {
		RecordBindings(vk_buffer, vk::PipelineBindPoint::eGraphics, pipeline);
		if (!mesh_active) {
			CommitIndexBuffer(vk_buffer, index_binding);
		}
		const auto draw_dwords = ShaderRecompiler::IR::PushData::GraphicsDrawDwords(
		    mesh_active, state.ps_input_info.object_primitive_id);
		if (draw_dwords != 0) {
			auto draw_data = mesh_draw_data;
			if (!mesh_active) {
				draw_data    = {};
				draw_data[0] = buffer.GetUserConfig().GetObjectId();
			}
			vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_constant_stages, 0,
			                        draw_dwords * sizeof(uint32_t), draw_data.data());
		}
	}

	auto& dynamic_state = m_context.GetCommandScheduler().GetDynamicState();
	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(), state.depth_info,
	                           rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	dynamic_state.Commit(m_context.GetGraphics(), vk_buffer);

	// DS shadow casters supply a separate PA_SU_POLY_OFFSET_* for front and
	// back faces. When this title-scoped path is enabled the pipeline exposes
	// dynamic culling, which must be explicitly set on *every* draw (including
	// ordinary draws) to avoid inheriting the preceding split face.
	static const bool ds_per_face_bias = [] {
		const char* setting = std::getenv("KYTY_DS_PER_FACE_BIAS");
		return setting != nullptr && setting[0] == '1' && setting[1] == '\0';
	}();
	const auto& face_mode = buffer.GetRegisters().GetModeControl();
	const auto& face_offset = buffer.GetRegisters().GetPolyOffset();
	const bool rect_face = Prospero::IsRectList(ucfg.GetPrimType());
	const bool front_bias = face_mode.poly_offset_front_enable && !face_mode.cull_front;
	const bool back_bias  = face_mode.poly_offset_back_enable && !face_mode.cull_back;
	if (ds_per_face_bias) {
		vk::CullModeFlags base_cull {};
		if (!rect_face && face_mode.cull_front) base_cull |= vk::CullModeFlagBits::eFront;
		if (!rect_face && face_mode.cull_back) base_cull |= vk::CullModeFlagBits::eBack;
		vk_buffer.setCullMode(base_cull);
		vk_buffer.setDepthBiasEnable(front_bias || back_bias);
		if (front_bias || back_bias) {
			const float guest_constant = front_bias ? face_offset.front_offset : face_offset.back_offset;
			const float constant = ConvertPolygonOffsetConstantFactor(
			    guest_constant, face_offset, state.depth_info.desc.view_info.format);
			const float slope = (front_bias ? face_offset.front_scale : face_offset.back_scale) / 16.0f;
			vk_buffer.setDepthBias(constant, face_offset.clamp, slope);
		}
	}

	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::Draws);
	m_context.GetCommandScheduler().NoteWork();
	if (Common::PerfStats::Enabled()) {
		const auto& named = state.ps_active ? state.ps_input_info.stage : state.vertex_info[0].stage;
		const uint64_t indices = mesh_emulated ? uint64_t {emulated_mesh.index_count}
		                                       : uint64_t {draw.index_count} * draw.instance_count;
		buffer.NotePassDraw(named.program != nullptr ? named.program->shader_hash : 0, indices);
		// KYTY_DRAW_LOG=<hash prefixes>: every 8th draw of those pixel (or vertex) shaders, at
		// most 400 lines: the parameters behind the pass labels ("Kindices" is 0 for indirect).
		static const std::vector<std::string> draw_log = [] {
			const char* spec = std::getenv("KYTY_DRAW_LOG");
			return spec != nullptr ? PipelineStats::ParseSpec(spec) : std::vector<std::string> {};
		}();
		static uint32_t sampled = 0;
		if (!draw_log.empty() && named.program != nullptr &&
		    PipelineStats::Matches(draw_log, named.program->shader_hash) && sampled++ % 8u == 0u &&
		    sampled / 8u < 400u) {
			char line[256];
			std::snprintf(line, sizeof(line),
			              "[perf-draw] hash=0x%016" PRIx64 " vs=0x%016" PRIx64
			              " %s count=%u instances=%u prim=%u indirect=%d mesh=%d/%d gs=%d %ux%u",
			              named.program != nullptr ? named.program->shader_hash : 0,
			              state.vertex_info[0].stage.program != nullptr
			                  ? state.vertex_info[0].stage.program->shader_hash : 0,
			              draw.Name(), draw.index_count, draw.instance_count,
			              static_cast<uint32_t>(ucfg.GetPrimType()), indirect != nullptr ? 1 : 0,
			              mesh_emulated ? 1 : 0, mesh_active ? 1 : 0,
			              state.ps_active && state.ps_input_info.stage.program != nullptr &&
			                      state.ps_input_info.stage.program->info.barycentric_emulation.enabled
			                  ? 1 : 0,
			              rendering.width, rendering.height);
			Common::PerfStats::EmitLine(line);
		}
	}
	const bool draw_marks = Common::PerfStats::Enabled() && LiveKnobs::DrawMarks();
	if (draw_marks) {
		// Closes the time since the previous mark (the pass begin or the previous draw's mark)
		// so the next mark times this draw alone.
		buffer.MarkGpu("misc");
	}
	const auto* label_vs = vertex_stages.empty() ? nullptr : vertex_stages.front().stage.program;
	const auto* label_ps = state.ps_active ? state.ps_input_info.stage.program : nullptr;
	{
		const ScopedDebugLabel draw_label(vk_buffer, mesh_emulated ? "draw-mesh" : "draw",
		                                  label_vs != nullptr ? label_vs->shader_hash : 0,
		                                  label_ps != nullptr ? label_ps->shader_hash : 0);
		const auto issue_draw = [&] {
			if (mesh_emulated && indirect != nullptr) {
				vk_buffer.drawIndexedIndirect(emulated_mesh.draw_buffer, emulated_mesh.draw_offset, 1,
				                              5 * sizeof(uint32_t));
			} else if (indirect != nullptr) {
				m_indirect_fixup->Draw(vk_buffer, indirect_records, indirect->indexed);
			} else if (mesh_emulated) {
				vk_buffer.drawIndexed(emulated_mesh.index_count, 1, 0, 0, 0);
			} else if (mesh_active) {
				vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
			} else {
				EmitDrawPrimitives(ucfg, vk_buffer, draw, emit);
			}
		};
		// With no face culling, a single VkPipeline cannot assign independent
		// polygon offset to each face. Split only depth-only draws without
		// stencil tests or shader buffer writes, so ordering-sensitive color
		// blending and storage side effects are untouched.
		const bool different_bias = face_mode.poly_offset_front_enable !=
		                                face_mode.poly_offset_back_enable ||
		    (face_mode.poly_offset_front_enable &&
		     (face_offset.front_offset != face_offset.back_offset ||
		      face_offset.front_scale != face_offset.back_scale));
		const bool split_bias = ds_per_face_bias && different_bias && !rect_face &&
		    !face_mode.cull_front && !face_mode.cull_back &&
		    state.depth_info.image_id && state.depth_info.depth_write_enable &&
		    state.color_count == 0 && !state.depth_info.stencil_test_enable &&
		    !mesh_active && !mesh_emulated && !shader_write_stages && !image_side_effects;
		if (split_bias) {
			const auto draw_face = [&](bool front) {
				const bool active = front ? face_mode.poly_offset_front_enable
				                          : face_mode.poly_offset_back_enable;
				vk_buffer.setCullMode(front ? vk::CullModeFlagBits::eBack
				                             : vk::CullModeFlagBits::eFront);
				vk_buffer.setDepthBiasEnable(active);
				if (active) {
					const float guest_bias = front ? face_offset.front_offset
					                               : face_offset.back_offset;
					const float constant = ConvertPolygonOffsetConstantFactor(
					    guest_bias, face_offset, state.depth_info.desc.view_info.format);
					const float slope = (front ? face_offset.front_scale
					                           : face_offset.back_scale) / 16.0f;
					vk_buffer.setDepthBias(constant, face_offset.clamp, slope);
				}
				issue_draw();
			};
			draw_face(true);
			draw_face(false);
			static std::atomic_uint32_t ds_split_logs {0};
			const uint32_t n = ds_split_logs.fetch_add(1, std::memory_order_relaxed);
			if (n < 8u || (n % 4096u) == 0u) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_PER_FACE_BIAS] split={} frame={} depth=0x{:x} "
				    "front={}/{} back={}/{} draw={} indirect={}\n",
				    n, m_context.GetGpu().GetFrameNum(),
				    state.depth_info.desc.info.data.address,
				    face_offset.front_offset, face_offset.front_scale,
				    face_offset.back_offset, face_offset.back_scale,
				    draw.index_count, indirect != nullptr ? 1 : 0));
			}
		} else {
			issue_draw();
		}
		if (coverage_pool) {
			const std::array<vk::Bool32, RENDER_COLOR_ATTACHMENTS_MAX> no_writes {};
			vk_buffer.setColorWriteEnableEXT(rendering.num_color_attachments, no_writes.data());
			vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, 0u);
			const auto query = [&](vk::CompareOp op) {
				vk_buffer.setDepthCompareOp(op);
				vk_buffer.beginQuery(coverage_pool, coverage_count, {});
				issue_draw();
				vk_buffer.endQuery(coverage_pool, coverage_count++);
			};
			query(vk::CompareOp::eEqual);
			query(vk::CompareOp::eAlways);
			if (dynamic_state.num_scissors == 1 && !m_pass_scaled && !ds_albedo_before.words.empty()) {
				const auto original = dynamic_state.scissors[0];
				const auto pixels = DemonsQueueProbe::MaterialCoveragePixels(
				    ds_albedo_before.width, ds_albedo_before.height,
				    static_cast<uint32_t>(ds_gbuffer_frame) / 240u);
				for (const auto& point : pixels) {
					const auto [x, y] = point;
					if (x >= rendering.width || y >= rendering.height ||
					    !DemonsQueueProbe::PixelInScissor(point, original.offset.x,
					        original.offset.y, original.extent.width, original.extent.height)) continue;
					const vk::Rect2D one {{static_cast<int32_t>(x), static_cast<int32_t>(y)}, {1, 1}};
					vk_buffer.setScissorWithCount(1, &one);
					query(vk::CompareOp::eEqual);
					query(vk::CompareOp::eAlways);
					coverage_points[coverage_points_used++] = point;
				}
				vk_buffer.setScissorWithCount(1, &original);
			}
			vk_buffer.setDepthCompareOp(state.depth_info.depth_compare_op);
			vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
			                              dynamic_state.stencil_front_write_mask);
			vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eBack,
			                              dynamic_state.stencil_back_write_mask);
			vk_buffer.setColorWriteEnableEXT(rendering.num_color_attachments,
			                                 dynamic_state.color_write_enables.data());
		}
	}

	if (draw_marks) {
		MarkDrawForLiveKnobs(buffer, state, vertex_stages, rendering, mesh_emulated,
		                     mesh_emulated ? uint64_t {emulated_mesh.index_count}
		                                   : uint64_t {draw.index_count} * draw.instance_count);
	}
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	if (shader_write_stages) {
		m_context.GetCommandScheduler().EndRendering(PassEnd::ShaderWrite);
		ShaderWriteBarrier(buffer.Handle(), shader_write_stages);
	}
	if (ds_gbuffer) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_GBUFFER_DRAW] frame={} phase=after shader=0x{:016x}\n",
		    ds_gbuffer_frame, ds_gbuffer_shader));
		const auto after = SnapshotDsAlbedo(m_context, *ds_gbuffer);
		ds_material_trace->content_version =
		    m_context.GetTextureCache().GetImage(ds_gbuffer->image_id).ContentVersion();
		std::array<uint64_t, CoverageQueries> hits {};
		if (coverage_pool) {
			if (after.words.empty()) m_context.GetCommandScheduler().Finish();
			const auto result = device.getQueryPoolResults(
			    coverage_pool, 0, coverage_count, coverage_count * sizeof(uint64_t), hits.data(),
			    sizeof(uint64_t), vk::QueryResultFlagBits::e64);
			device.destroyQueryPool(coverage_pool);
			if (result != vk::Result::eSuccess) {
				coverage_points_used = 0;
				hits = {};
			}
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_GBUFFER_COVERAGE] frame={} shader=0x{:016x} result={} equal_any={} always_any={}\n",
			    ds_gbuffer_frame, ds_gbuffer_shader, vk::to_string(result), hits[0] != 0, hits[1] != 0));
		}
		RecordDsMaterialPixels(*ds_material_trace, ds_albedo_before, after,
		                       std::span{coverage_points.data(), coverage_points_used}, hits,
		                       rendering.color_attachments[ds_gbuffer->target_slot].is_clear);
	}
	if (ds_post) {
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		for (const auto& color : std::span{state.color_info, state.color_count}) {
			if (!color.image_id || (!ds_post_final &&
			    !ds_post->TracesImage(color.desc.info.data.address))) continue;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_POST_TARGET] light_frame={} pass={} phase=after guest=0x{:x} slot={} load_clear={}\n",
			    ds_post->frame, ds_post->post_passes, color.desc.info.data.address, color.target_slot,
			    rendering.color_attachments[color.target_slot].is_clear));
			ProbeDsColorOutputs(m_context, &color, 1, shader, false, true, nullptr,
			                    &ds_post->pixels, ds_post->width, ds_post->height);
		}
		if (ds_post_final) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_POST_COMPLETE] light_frame={} frame={} passes={}\n",
			    ds_post->frame, m_context.GetGpu().GetFrameNum(), ds_post->post_passes));
		}
	}
	for (size_t i = 0; i < ds_draw_stage.size(); ++i) {
		const auto stage = ds_draw_stage[i];
		if (!stage) continue;
		auto& capture = m_ds_queue_captures[i];
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_QUEUE_DRAW] frame={} queue={} phase={} shader=0x{:016x} hdr_writer={} hdr_reader={} composite={}\n",
		    capture.frame, DemonsQueueProbe::Queues[i],
		    (stage & DsComposite) ? "composite_output" :
		    (stage & DsHdrWriter) ? "hdr_writer_after" : "hdr_reader_output", shader,
		    (stage & DsHdrWriter) != 0, (stage & DsHdrReader) != 0, (stage & DsComposite) != 0));
		ProbeDsColorOutputs(m_context, state.color_info, state.color_count, shader, false, true,
		                    nullptr, &capture.pixels, capture.width, capture.height);
		capture.hdr_reader_sampled |= (stage & DsHdrReader) != 0;
		capture.hdr_writer_sampled |= (stage & DsHdrWriter) != 0;
		capture.composite_sampled |= (stage & DsComposite) != 0;
	}
	if (ds_post && ds_post_final) ds_post->composite_sampled = true;
	if (ds_scene_trace) {
		const auto frame = m_context.GetGpu().GetFrameNum();
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		const auto after = SnapshotDsSceneGrid(
		    m_context, ds_scene_target.image_id, ds_scene_target.desc.view_info);
		uint32_t changed_cells = 0;
		if (ds_scene_before.valid && after.valid) {
			for (uint32_t i = 0; i < 64u; ++i) {
				changed_cells += !std::equal(ds_scene_before.rgba.begin() + i * 4u,
				                             ds_scene_before.rgba.begin() + (i + 1u) * 4u,
				                             after.rgba.begin() + i * 4u);
			}
		}
		const auto& regs = buffer.GetRegisters();
		const auto& blend = regs.GetBlendControl(ds_scene_target.target_slot);
		const auto& cc = regs.GetColorControl();
		const auto& sh = regs.GetShaderRegisters();
		const auto& dyn = m_context.GetCommandScheduler().GetDynamicState();
		const vk::Rect2D scissor = dyn.num_scissors ? dyn.scissors[0] : vk::Rect2D {};
		nlohmann::json payload {
		    {"frame", frame}, {"pass", ds_scene_pass}, {"shader", fmt::format("{:016x}", shader)},
		    {"guest", ds_scene_target.desc.info.data.address},
		    {"target_slot", ds_scene_target.target_slot},
		    {"bright_baseline", DemonsAdaptiveCapture::Current().BrightSceneActive(frame)},
		    {"paired_event", DemonsAdaptiveCapture::Current().CapturedFrame(frame)},
		    {"before", DsSceneGridJson(ds_scene_before)}, {"after", DsSceneGridJson(after)},
		    {"changed_cells", changed_cells}, {"inputs", ds_scene_inputs},
		    {"raster", {
		        {"indices", draw.index_count}, {"instances", draw.instance_count},
		        {"indexed", draw.IsIndexed()}, {"indirect", indirect != nullptr},
		        {"mesh", mesh_active}, {"render_width", rendering.width},
		        {"render_height", rendering.height},
		        {"target_mask", regs.GetRenderTargetMask()},
		        {"shader_mask", sh.m_cbShaderMask},
		        {"pixel_output_mode", state.ps_input_info.target_output_mode[ds_scene_target.target_slot]},
		        {"pixel_kill", state.ps_input_info.ps_pixel_kill_enable},
		        {"color_control_mode", cc.mode}, {"color_control_op", cc.op},
		        {"blend", blend.enable}, {"blend_src", blend.color_srcblend},
		        {"blend_dst", blend.color_destblend}, {"blend_op", blend.color_comb_fcn},
		        {"depth_test", state.depth_info.depth_test_enable},
		        {"depth_write", state.depth_info.depth_write_enable},
		        {"depth_compare", static_cast<uint32_t>(state.depth_info.depth_compare_op)},
		        {"stencil_test", state.depth_info.stencil_test_enable},
		        {"scissor", {scissor.offset.x, scissor.offset.y,
		                    scissor.extent.width, scissor.extent.height}},
		        {"native_color_write", static_cast<bool>(
		            dyn.color_write_enables[ds_scene_target.target_slot])},
		        {"load_clear", rendering.color_attachments[ds_scene_target.target_slot].is_clear}
		    }}
		};
		const auto folder = Config::GetShaderDumpFolder() / "queue-state";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		const auto name = fmt::format("ds_scene_writer_f{}_p{}.json", frame, ds_scene_pass);
		std::ofstream file(folder / name, std::ios::trunc);
		if (file) file << payload;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_SCENE_WRITE] frame={} pass={} shader=0x{:016x} before={:.7f} "
		    "after={:.7f} lit={}/{} changed_cells={} blend={} depth={} stencil={} "
		    "write={} clear={} file={} saved={}\n",
		    frame, ds_scene_pass, shader, ds_scene_before.luminance, after.luminance,
		    ds_scene_before.lit, after.lit, changed_cells, blend.enable,
		    state.depth_info.depth_test_enable, state.depth_info.stencil_test_enable,
		    static_cast<bool>(dyn.color_write_enables[ds_scene_target.target_slot]),
		    rendering.color_attachments[ds_scene_target.target_slot].is_clear, name,
		    file.good()));
	}
	if (ds_live_composite_output) {
		const auto frame = m_context.GetGpu().GetFrameNum();
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_PAIRED_COMPOSITOR_OUTPUT] frame={} shader=0x{:016x} "
		    "targets={} target0=0x{:x}\n",
		    frame, shader, state.color_count, state.color_info[0].desc.info.data.address));
		ProbeDsColorOutputs(m_context, state.color_info, state.color_count,
		                    shader, false, true);
	}
	// Diagnose the Demon scene chain at most twice per run: the actual
	// sampled HUD/scene inputs versus the compositor's final color target.
	// One short set after initialization and another during live play. Normal
	// draw/compute probes are suppressed by KYTY_DS_COLOR_PROBE_TARGETED.
	static const bool ds_compositor_pixel_probe = [] {
		const char* v = std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");
		return v != nullptr && v[0] == '1' && v[1] == '\0';
	}();
	if (ds_compositor_pixel_probe && state.ps_active && bindings.pixel &&
	    state.ps_input_info.stage.program != nullptr &&
	    state.ps_input_info.stage.program->shader_hash == 0x3265b01ed00a5c01ull &&
	    state.color_count != 0u) {
		const int32_t frame = m_context.GetGpu().GetFrameNum();
		static DemonsRollingProbe::FrameGate composite_pixels;
		if (composite_pixels.Due(frame, 240)) {
			std::array<RenderColorInfo, 2> sources {};
			uint32_t count = 0;
			const auto& bound_images = bindings.pixel->images;
			for (uint32_t i = 0; i < bound_images.size() && count < sources.size(); ++i) {
				if (bound_images[i].image_id) {
					sources[count].image_id = bound_images[i].image_id;
					sources[count].desc = bound_images[i].desc;
					++count;
				}
			}
			const auto shader_hash = state.ps_input_info.stage.program->shader_hash;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_COMPOSITE_PIXEL_PHASE] frame={} capture=rolling sources={} outputs={}\n",
			    frame, count, state.color_count));
			if (count != 0u) {
				ProbeDsColorOutputs(m_context, sources.data(), count, shader_hash, false, true);
			}
			ProbeDsColorOutputs(m_context, state.color_info, state.color_count,
			                    shader_hash, false, true);
		}
	}
	// Continuously sample the three observed scene targets with per-target
	// spacing. If a session ends early or runs far longer, its most recent
	// capture is still present instead of expiring at a fixed phase.
	if (ds_compositor_pixel_probe && state.ps_active && state.color_count != 0u &&
	    state.ps_input_info.stage.program != nullptr &&
	    state.color_info[0].image_id) {
		const uint64_t target = state.color_info[0].desc.info.data.address;
		const int32_t frame = m_context.GetGpu().GetFrameNum();
		static DemonsRollingProbe::FrameGate scene_color;
		static DemonsRollingProbe::FrameGate scene_overlay;
		static DemonsRollingProbe::FrameGate scene_hdr;
		DemonsRollingProbe::FrameGate* gate = nullptr;
		if (target == 0x281300000ull) gate = &scene_color;
		if (target == 0x2b2a40000ull) gate = &scene_overlay;
		if (target == 0x287ae0000ull) gate = &scene_hdr;
		if (gate != nullptr && gate->Due(frame, 240)) {
			const auto shader_hash = state.ps_input_info.stage.program->shader_hash;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_SCENE_PIXEL_PHASE] frame={} capture=rolling target=0x{:x} shader=0x{:016x}\n",
			    frame, target, shader_hash));
			ProbeDsColorOutputs(m_context, state.color_info, state.color_count,
			                    shader_hash, false, true);
		}
	}
	// Only a deliberate DS probe samples pixels. The ordinary renderer never
	// pays a forced submission boundary or a host-visible image readback.
	if (state.ps_active && state.color_count != 0u) {
		const uint64_t ps_hash = state.ps_input_info.stage.program != nullptr
		    ? state.ps_input_info.stage.program->shader_hash : 0;
		ProbeDsColorOutputs(m_context, state.color_info, state.color_count, ps_hash);
	}
	// PPSA01341: distinguish accepted depth-only draws from a shadow atlas
	// that was never written. These counters never force a GPU wait.
	static const bool ds_depth_trace = [] {
		const char* env = std::getenv("KYTY_DS_FRAME_TRACE");
		return env != nullptr && env[0] == '1' && env[1] == '\0';
	}();
	if (ds_depth_trace && state.color_count == 0u && state.depth_info.image_id) {
		static std::atomic_uint32_t depth_events {0};
		static std::atomic_uint32_t depth_102a4 {0};
		static std::atomic_uint32_t depth_20926 {0};
		const uint32_t n = depth_events.fetch_add(1u, std::memory_order_relaxed);
		const auto& di = state.depth_info;
		const uint64_t address = di.desc.info.data.address;
		if (address == 0x102a400000ull) depth_102a4.fetch_add(1u, std::memory_order_relaxed);
		if (address == 0x209260000ull) depth_20926.fetch_add(1u, std::memory_order_relaxed);
		if (n < 32u || n % 4096u == 0u) {
			const auto& tracked = m_context.GetTextureCache().GetImage(di.image_id);
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_DEPTH_DRAW] seq={} frame={} target=0x{:x} format={} extent={}x{} "
			    "depth_write={} depth_test={} gpu_modified={} cpu_dirty={} "
			    "content_version={} seen_102a4={} seen_20926={}\n",
			    n, m_context.GetGpu().GetFrameNum(), address,
			    static_cast<uint32_t>(tracked.backing.format),
			    tracked.backing.extent.width, tracked.backing.extent.height,
			    di.depth_write_enable ? 1 : 0, di.depth_test_enable ? 1 : 0,
			    tracked.IsGpuModified() ? 1 : 0, tracked.IsCpuDirty() ? 1 : 0,
			    tracked.ContentVersion(), depth_102a4.load(std::memory_order_relaxed),
			    depth_20926.load(std::memory_order_relaxed)));
		}
	}
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
}

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopDrawOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset) ||
	    DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		return;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	// Only the sampled hot indexed draws pay a few extra clock reads.
	// This measures actual Android gameplay rather than blaming the total
	// PM4 handler on shader preparation, primitive restart or Vulkan alone.
	using DrawClock = std::chrono::steady_clock;
	const bool sample_draw = UfcPerf::SampleDrawPath();
	const auto time0 = sample_draw ? DrawClock::now() : DrawClock::time_point {};
	const bool primitive_restart = ResolvePrimitiveRestart(buffer, index_source);
	const auto time1 = sample_draw ? DrawClock::now() : DrawClock::time_point {};
	if (sample_draw) {
		UfcPerf::Add(UfcPerf::Site::DrawPrimitive, static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(time1 - time0).count()));
	}

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance};
	auto& state = ReusableDrawRenderState();
	const bool draw_ready = PrepareDrawRenderState(
	    buffer, draw, args.render_target_slice_offset, state);
	const auto time2 = sample_draw ? DrawClock::now() : DrawClock::time_point {};
	if (sample_draw) {
		UfcPerf::Add(UfcPerf::Site::DrawPrepare, static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(time2 - time1).count()));
	}
	if (!draw_ready) {
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);

	DrawEmitInfo emit {};
	// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
	const bool native_indirect = indirect && !state.vertex_info[0].fetch_embedded;
	emit.vertex_offset  = native_indirect ? 0 : vertex_offset + args.base_vertex;
	emit.first_instance = native_indirect ? 0 : instance_offset;

	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
	                    primitive_restart);
	const auto time3 = sample_draw ? DrawClock::now() : DrawClock::time_point {};
	if (sample_draw) {
		UfcPerf::Add(UfcPerf::Site::DrawExecute, static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(time3 - time2).count()));
	}
	ResetBindings();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopDrawOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset) ||
	    DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance};

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		ResetBindings();
		return;
	}
	auto& state = ReusableDrawRenderState();
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		ResetBindings();
		return;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto [vertex_offset, instance_offset] =
	    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
	             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);
	DrawEmitInfo emit {};
	const bool native_indirect = indirect && !state.vertex_info[0].fetch_embedded;
	emit.first_vertex = native_indirect ? 0 :
	    static_cast<uint32_t>(vertex_offset + static_cast<int32_t>(args.first_vertex));
	emit.first_instance = native_indirect ? 0 : instance_offset;

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false);
	ResetBindings();
}

// --gpu-indirect: why a packet keeps the CPU path (nullptr if it does not). Decided from registers
// alone: nothing is recorded and no guest memory is read before it.
static const char* IndirectDrawUnsupported(const CommandBuffer& buffer, const IndirectDrawArgs& args) {
	constexpr uint64_t MaxIndexBytes = 64ull * 1024 * 1024;
	if (!buffer.GetGraphics().optional_features.gpu_indirect_draws) {
		return "device lacks shaderDrawParameters or drawIndirectFirstInstance";
	}
	if (IndirectDrawFixup::GuestRecordsSize(args) == 0 ||
	    (args.count_addr != 0 &&
	     LibKernel::Memory::ClampRangeSize(args.count_addr, sizeof(uint32_t)) != sizeof(uint32_t))) {
		return "records are not mapped guest memory";
	}
	const auto& hw   = buffer.GetRegisters();
	const auto  mode = hw.GetColorControl().mode;
	if (mode != static_cast<uint8_t>(CbColorMode::Disable) &&
	    mode != static_cast<uint8_t>(CbColorMode::Normal)) {
		return "color metadata operation";
	}
	const auto& override = hw.GetDepthRenderOverride();
	if (override.force_z_dirty || override.force_stencil_dirty ||
	    hw.GetRenderControl().copy_depth_to_color) {
		return "depth copy";
	}
	const auto& ucfg = buffer.GetUserConfig();
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return "primitive type";
	}
	if (!args.indexed) {
		return nullptr;
	}
	const auto type = static_cast<Prospero::IndexType>(args.index_type_and_size);
	if (type != Prospero::IndexType::kIndex16 && type != Prospero::IndexType::kIndex32) {
		return "8-bit indices";
	}
	const uint64_t element = type == Prospero::IndexType::kIndex32 ? 4u : 2u;
	const uint64_t bytes   = uint64_t {args.index_buffer_size} * element;
	if (args.index_base_addr == 0 || bytes == 0 || bytes > MaxIndexBytes) {
		return "index buffer size";
	}
	// ResolvePrimitiveRestart scans the indices for a custom reset value.
	const auto control = ucfg.GetPrimitiveResetControl();
	const bool strip   = ucfg.GetPrimType() != Prospero::PrimitiveType::kPointList &&
	                   ucfg.GetPrimType() != Prospero::PrimitiveType::kLineList &&
	                   ucfg.GetPrimType() != Prospero::PrimitiveType::kTriList;
	const auto mask  = static_cast<uint32_t>(UINT32_MAX >> ((4u - element) * 8u));
	const auto reset = hw.GetPrimitiveResetIndex();
	if ((control & 0x1u) != 0 && strip && !((control & 0x2u) != 0 && (reset & ~mask) != 0) &&
	    (reset & mask) != mask) {
		return "custom primitive restart index";
	}
	return nullptr;
}

static void LogIndirectFallback(const char* reason) {
	static std::mutex                      mutex;
	static std::vector<const char*>        logged;
	std::lock_guard                        lock(mutex);
	if (std::ranges::find(logged, reason) == logged.end()) {
		logged.push_back(reason);
		LOGF("GPU indirect draw: CPU path for %s\n", reason);
	}
}

bool RenderExecutor::DrawIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                  const IndirectDrawArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid() || !args.sgprs.enabled);
	if (const char* reason = IndirectDrawUnsupported(buffer, args); reason != nullptr) {
		LogIndirectFallback(reason);
		return false;
	}
	m_context.GetCommandScheduler().PopDrawOperations();
	auto& ucfg = buffer.GetUserConfig();
	const auto debug_op =
	    args.indexed ? CommandBufferDebugOp::DrawIndex : CommandBufferDebugOp::DrawIndexAuto;
	buffer.SetDebugInfo(static_cast<uint32_t>(debug_op), submit_id, 0, 0, args.max_count, 0,
	                    args.args_addr);

	Common::LockGuard lock(m_context.GetMutex());
	if (!DrawHasValidVertexShader(buffer.GetShaders())) {
		return true;
	}
	uc_check(ucfg);
	hw_check(buffer);
	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		return true;
	}

	DrawIndexBufferSource index_source {};
	if (args.indexed) {
		const bool wide                 = args.index_type_and_size ==
		                  static_cast<uint32_t>(Prospero::IndexType::kIndex32);
		index_source.address            = args.index_base_addr;
		index_source.type               = wide ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
		index_source.guest_element_size = wide ? 4u : 2u;
		index_source.size = LibKernel::Memory::ClampRangeSize(
		    args.index_base_addr,
		    uint64_t {args.index_buffer_size} * index_source.guest_element_size);
	}
	const bool primitive_restart = args.indexed && ResolvePrimitiveRestart(buffer, index_source);

	// Counts are unknown here: debug output and statistics see the record count.
	const DrawCallInfo draw {debug_op, args.max_count, 1, 0};
	auto& state = ReusableDrawRenderState();
	state.indirect = args.sgprs;
	if (!PrepareDrawRenderState(buffer, draw, 0, state)) {
		ResetBindings();
		return true;
	}
	const auto& vertex = state.vertex_info[0];
	const char* stage_reason = nullptr;
	if (state.programs.VertexStageCount() != 1 || !vertex.indirect.enabled) {
		stage_reason = vertex.logical_stage == ShaderType::Mesh ? "native or fast-launch mesh stage"
		                                                         : "tessellation";
	} else if (vertex.stage.program->stage == ShaderType::Mesh &&
	           (!args.indexed || args.max_count != 1 || args.count_addr != 0)) {
		// An emulated mesh draw sizes its outputs from INDEX_BUFFER_SIZE, one record at a time.
		stage_reason = "emulated mesh stage without one indexed record";
	}
	if (stage_reason != nullptr) {
		ResetBindings();
		LogIndirectFallback(stage_reason);
		return false;
	}
	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     reinterpret_cast<const void*>(args.index_base_addr));
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, DrawEmitInfo {}, index_source,
	                    primitive_restart, &args);
	ResetBindings();
	return true;
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	RememberDsAtlasWriter(dst.image_id, dst.desc.info, dst.desc.view_info, 0, "resolve");
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	// The resolve addresses guest texels of both images.
	source.SetScaled(false);
	destination.SetScaled(false);
	auto* post = dst.desc.info.data.address == DemonsQueueProbe::SceneAddress ? DsPostCapture() : nullptr;
	if (post && !DemonsQueueProbe::TracePostPass(post->post_passes, post->scene_passes, true, false))
		post = nullptr;
	if (post) {
		++post->post_passes;
		++post->scene_passes;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_POST_RESOLVE] light_frame={} frame={} pass={} phase=before source=0x{:x} target=0x{:x}\n",
		    post->frame, m_context.GetGpu().GetFrameNum(), post->post_passes,
		    src.desc.info.data.address, dst.desc.info.data.address));
		ProbeDsColorOutputs(m_context, &src, 1, 0, false, true, &src.desc.view_info,
		                    &post->pixels, post->width, post->height);
	}
	// A target can live inside a larger image: GTA V resolves each face of its reflection cube by
	// pointing CB_COLOR1 at that face's slice. FindImage() then moved the view to the matching
	// level and layer of that image, which is where the resolve has to read and write.
	destination.Resolve(source,
	                    {src.desc.view_info.base_level, 1, src.desc.view_info.base_layer, 1},
	                    {dst.desc.view_info.base_level, 1, dst.desc.view_info.base_layer, 1});
	if (post) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_POST_RESOLVE] light_frame={} frame={} pass={} phase=after target=0x{:x}\n",
		    post->frame, m_context.GetGpu().GetFrameNum(), post->post_passes, dst.desc.info.data.address));
		ProbeDsColorOutputs(m_context, &dst, 1, 0, false, true, &dst.desc.view_info,
		                    &post->pixels, post->width, post->height);
	}
	return true;
}

} // namespace Libs::Graphics
