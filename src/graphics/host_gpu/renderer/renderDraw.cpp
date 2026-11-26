#include "graphics/shader/demonsPcRenderParity.h"
#include "graphics/host_gpu/renderer/demonsPrelightTrace.h"
#include "graphics/host_gpu/renderer/demonsIndirectWitness.h"
#include "kernel/addressFold.h"
#include "graphics/host_gpu/renderer/demonsDominantGeometry.h"
#include "graphics/host_gpu/renderer/pipeline/demonsVertexPositionProbe.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/host_gpu/renderer/demonsRollingProbe.h"
#include "graphics/host_gpu/renderer/demonsAdaptiveCapture.h"
#include "graphics/host_gpu/renderer/demonsDepthEqualPolicy.h"
#include "graphics/host_gpu/renderer/demonsMaterialFullFramePolicy.h"
#include "graphics/host_gpu/renderer/demonsDepthFullCoveragePolicy.h"
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
#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"
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
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

static std::optional<std::array<uint32_t,64>> SnapshotDsGBufferGrid(RenderContext&,const RenderColorInfo&);

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
                                       const ShaderVertexInputInfo& vs_input_info, uint64_t pixel_shader_hash,
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
	// Preserve the guest depth comparison by default. The scoped automatic
	// GREATER_OR_EQUAL experiment did not restore the scenery on device.
	// Explicit launcher modes 1/2 remain opt-in for A/B comparison.
	static const int ds_depth_equal_mode = [] {
		const char* value = std::getenv("KYTY_DS_DEPTH_EQUAL_RELAX");
		if (value == nullptr || value[1] != '\0' || value[0] < '0' || value[0] > '3')
			return 0;
		return value[0] - '0';
	}();
	auto depth_compare = depth.depth_compare_op;
	const bool stencil_always = !depth.stencil_test_enable ||
	    (depth.stencil_front.compareOp == vk::CompareOp::eAlways &&
	     depth.stencil_back.compareOp == vk::CompareOp::eAlways);
	const auto decision = DemonsDepthEqualPolicy::Select(
	    DemonsPcRenderParity::Enabled() ? 0 : ds_depth_equal_mode, pixel_shader_hash, depth.desc.info.data.address,
	    depth.depth_test_enable, depth.depth_write_enable, stencil_always,
	    static_cast<uint32_t>(depth_compare));
	if (decision != DemonsDepthEqualPolicy::Decision::Guest) {
		depth_compare = decision == DemonsDepthEqualPolicy::Decision::LessOrEqual
		    ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eGreaterOrEqual;
		static std::atomic<uint32_t> logged {0};
		const uint32_t bit = pixel_shader_hash == DemonsDepthEqualPolicy::SuspectScene0 ? 1u
		    : pixel_shader_hash == DemonsDepthEqualPolicy::SuspectScene1 ? 2u : 4u;
		if ((logged.fetch_or(bit) & bit) == 0u)
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_GBUFFER_DEPTH_EQUAL] material=0x{:016x} original=EQUAL effective={} mode={} "
			    "scope=scenery_readonly depth=0x{:x}\n",
			    pixel_shader_hash,
			    decision == DemonsDepthEqualPolicy::Decision::LessOrEqual ? "LESS_OR_EQUAL" :
			        "GREATER_OR_EQUAL", ds_depth_equal_mode, depth.desc.info.data.address));
	}
	dynamic_state.SetDepthCompareOp(depth_compare);
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
		const bool root_acquire=DemonsPrelightTrace::active_draw &&
            target.desc.info.data.address==DemonsQueueProbe::GBufferAddress &&
            target.desc.view_info.base_layer==0 && target.desc.view_info.base_level==0;
        const bool root_dirty=root_acquire&&(owner->IsBufferModified()||owner->IsCpuDirty());
        const auto root_prior=root_dirty?DemonsPrelightTrace::Identity(*owner):nlohmann::json();
        auto  image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto& image      = cache.GetImage(target.image_id);
		ensure_pass_scale(image, target.desc.view_info, image_view);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto shader = stages.empty() ? 0u : stages.back()->runtime->program->shader_hash;
        // AFTER RefreshImage/UploadImage, BEFORE attachment transition/loadOp/draw.
        if(root_acquire&&(root_dirty||shader==DemonsDominantGeometry::MaterialPs)){
            nlohmann::json row={{"kind","draw_acquired"},{"draw",DemonsPrelightTrace::active_draw},
                {"frame",m_context.GetGpu().GetFrameNum()},{"tick",m_context.GetCommandScheduler().CurrentTick()},
                {"shader",shader},{"dirty_before",root_dirty},{"prior_image",root_prior},
                {"image",DemonsPrelightTrace::Identity(image)},{"layer",view.base_layer},{"mip",view.base_level},
                {"pending_fold",image.rp_fold.Pending()}};
            if(!image.rp_fold.Pending()&&DemonsPrelightTrace::AllowSample()){
                const auto sample=SnapshotDsGBufferGrid(m_context,target);
                row["grid"]=sample?nlohmann::json(*sample):nlohmann::json();
            }else row["sample_status"]="pending_fold_or_budget";
            row["tick_after_sample"]=m_context.GetCommandScheduler().CurrentTick();
            DemonsPrelightTrace::Record(std::move(row));
        }
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
		// PC renderer parity: a guest depth clear invalidates the previous HTile
		// fast-clear identity and publishes a fresh metadata clear before attachment
		// acquisition. Without it the later depth-only/material passes can reload
		// stale or zero depth, even though their draw shaders are valid.
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		uint32_t htile_fill = 0;
		bool htile_fill_known = false;
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer,
		                        &htile_fill, &htile_fill_known);
		// PC-compatible HTile load: ZMask=0xf preserves depth.
		// The previous Android path cleared D32 even on stencil-only fills.
		static const bool ds_htile_semantics = [] {
			const char* value = std::getenv("KYTY_DS_HTILE_FILL_SEMANTICS");
			return value != nullptr && value[0] == '1' && value[1] == '\0';
		}();
		const auto decision = DecideHtileLoad(meta_clear, htile_fill, htile_fill_known,
		                                      metadata.stencil_compressed);
		if(meta_clear&&depth.desc.info.data.address==0x282b60000ull&&DemonsPcRenderParity::Enabled()) {
			static const bool trace=[] {const char* v=std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");return v&&v[0]=='1';}();
			if(trace){const auto origin=cache.GetMetaClearOrigin(metadata.range.address);
				Log::WriteToConsoleAndLog(fmt::format("[DS_HTILE_ORIGIN] frame={} target=0x{:x} meta=0x{:x} slice={} source={} writer=0x{:016x} write_addr=0x{:x} write_bytes=0x{:x} meta_bytes=0x{:x} writer_tick={} consumer_tick={} fill_known={} fill=0x{:08x} guest_clear={}\n",m_context.GetGpu().GetFrameNum(),depth.desc.info.data.address,metadata.range.address,depth.desc.view_info.base_layer,origin.source,origin.shader,origin.address,origin.bytes,metadata.range.size,origin.tick,m_context.GetCommandScheduler().CurrentTick(),htile_fill_known,htile_fill,depth.depth_clear_enable));}
		}
		depth.trace_meta_pending = meta_clear;
		depth.trace_meta_known = htile_fill_known;
		depth.trace_meta_fill = htile_fill;
		depth.trace_meta_depth_clear = decision.depth_meta_clear;
		depth.trace_meta_stencil_clear = decision.stencil_meta_clear;
		depth.trace_meta_uniform = decision.depth_uniform;
		if (DemonsPcRenderParity::Enabled()) {
			depth.trace_meta_depth_clear = meta_clear;
			depth.trace_meta_stencil_clear = meta_clear && metadata.stencil_compressed;
			depth.trace_meta_uniform = false;
		}
		depth.load_clear_aspects = {};
		if (ds_htile_semantics && !DemonsPcRenderParity::Enabled()) {
			if (decision.depth_uniform) depth.depth_clear_value = decision.uniform_value;
			if (depth.depth_clear_enable || decision.depth_meta_clear)
				depth.load_clear_aspects |= vk::ImageAspectFlagBits::eDepth;
			if (depth.stencil_clear_enable || decision.stencil_meta_clear)
				depth.load_clear_aspects |= vk::ImageAspectFlagBits::eStencil;
		} else {
			if (depth.depth_clear_enable || meta_clear)
				depth.load_clear_aspects |= vk::ImageAspectFlagBits::eDepth;
			if (depth.stencil_clear_enable || (meta_clear && metadata.stencil_compressed))
				depth.load_clear_aspects |= vk::ImageAspectFlagBits::eStencil;
		}
		// Preserve legacy depth-only readers; the actual Vulkan attachment
		// and hazard policy below uses load_clear_aspects exclusively.
		depth.depth_load_clear_enable =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eDepth);
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
		// As in U59, loading an attachment without guest writes or clears
		// does not change its content identity. Determine this only after
		// consuming pending HTile clears, which can write either aspect.
		if (draw_writes) {
			cache.MarkDepthTargetWritten(depth.image_id);
		}
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
		attachment.depth_clear    =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eStencil);
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

// Read the exact bound view of the common scene-depth discard input. A
// raw guest backing dump is tiled guest memory, not a linear texel oracle.
static nlohmann::json SnapshotDsDiscardImage(RenderContext& context,
    const PreparedBindings& pixel, const std::array<uint32_t,8>& raw) {
    nlohmann::json result={{"status","unavailable"},{"guest_descriptor",raw}};
    if(!raw[0]) {result["status"]="null_or_zero_base";return result;}
    // The captured family uses a 32-bit base in units of 256 bytes. Fail
    // closed on high address bits; do not silently select another image.
    if(raw[1]&0x3fu){result["status"]="unsupported_high_address";return result;}
    const uint64_t expected=uint64_t{raw[0]}<<8;
    result["expected_guest"]=expected;
    result["bound_views"]=nlohmann::json::array();
    size_t matches=0;
    const auto& inputs=pixel.images;
    size_t selected=0;
    for(size_t i=0;i<inputs.size();++i){
        const auto& in=inputs[i];
        result["bound_views"].push_back({{"slot",i},{"guest",in.desc.info.data.address},
            {"placeholder",in.placeholder || !in.image_id},{"format",uint32_t(in.desc.view_info.format)},
            {"mip",in.desc.view_info.base_level},{"layer",in.desc.view_info.base_layer}});
        if(in.desc.info.data.address==expected && in.image_id && !in.placeholder){selected=i;++matches;}
    }
    if(matches!=1){result["status"]=matches?"ambiguous_binding":"no_exact_binding";return result;}
    const auto& input=inputs[selected];
    auto& image=context.GetTextureCache().GetImage(input.image_id);
    const auto found=std::ranges::find(image.views,input.image_view,&CachedImageView::view);
    if(found==image.views.end()){result["status"]="native_view_unavailable";return result;}
    const auto& view=found->info;
    const auto& backing=image.backing;
    result["native_slot"]=selected;
    result["owner"]=image.info.data.address;
    result["owner_bytes"]=image.info.data.size;
    result["version"]=image.ContentVersion();
    result["gpu_modified"]=image.IsGpuModified();
    result["cpu_dirty"]=image.IsCpuDirty();
    result["buffer_modified"]=image.IsBufferModified();
    result["view_format"]=uint32_t(view.format);
    result["backing_format"]=uint32_t(backing.format);
    result["view_mip"]=view.base_level;
    result["view_layers"]=view.layer_count;
    result["view_levels"]=view.level_count;
    result["view_layer"]=view.base_layer;
    result["view_mapping"]={uint32_t(view.mapping.r),uint32_t(view.mapping.g),uint32_t(view.mapping.b),uint32_t(view.mapping.a)};
    result["guest_format"]=uint32_t(image.info.guest_format);
    result["guest_tile_mode"]=uint32_t(image.info.tile_mode);
    result["guest_pitch"]=image.info.pitch;
    result["guest_mip_offset"]=view.base_level<image.info.mip_layout.size()?image.info.mip_layout[view.base_level].offset:0ull;
    if(image.info.data.size && image.info.data.size<=65536u){
        std::vector<uint8_t> bytes(image.info.data.size);
        if(LibKernel::Memory::TryReadBacking(image.info.data.address,bytes.data(),bytes.size()))
            result["guest_backing_bytes"]=std::move(bytes);
    }
    if(!backing.image || backing.image_type!=vk::ImageType::e2D || backing.samples!=1 ||
       !(backing.usage&vk::ImageUsageFlagBits::eTransferSrc) ||
       view.aspect!=vk::ImageAspectFlagBits::eColor || view.base_level>=backing.mip_levels ||
       view.base_layer>=backing.layers || backing.format==vk::Format::eUndefined){
        result["status"]="unsupported_native_view";return result;
    }
    const auto block=vk::blockExtent(backing.format);
    const uint32_t bpb=vk::blockSize(backing.format);
    result["block_extent"]={block[0],block[1],block[2]};
    result["bytes_per_block"]=bpb;
    // A compressed VkBufferImageCopy still uses the texel extent, but the
    // buffer occupies ceil(width/blockW)*ceil(height/blockH)*blockBytes.
    // Capture every accessible mip: implicit LOD can sample any of them.
    if(!bpb || !block[0] || !block[1] || block[2]!=1 ||
       !view.level_count || view.level_count>16 ||
       view.level_count>backing.mip_levels-view.base_level){
        result["status"]="bounded_readback_unsupported";return result;
    }
    std::vector<vk::BufferImageCopy> copies;
    nlohmann::json layout=nlohmann::json::array();
    uint64_t size=0;
    for(uint32_t i=0;i<view.level_count;++i){
        const auto level=view.base_level+i;
        const uint32_t width=std::max(1u,backing.extent.width>>level);
        const uint32_t height=std::max(1u,backing.extent.height>>level);
        const uint64_t bytes=((uint64_t{width}+block[0]-1)/block[0])*
                             ((uint64_t{height}+block[1]-1)/block[1])*bpb;
        size=(size+15u)&~uint64_t{15u};
        if(!bytes || bytes>65536u || size>65536u-bytes){
            result["status"]="bounded_readback_unsupported";return result;
        }
        vk::BufferImageCopy copy{};
        copy.bufferOffset=size;
        copy.imageSubresource={vk::ImageAspectFlagBits::eColor,level,view.base_layer,1};
        copy.imageExtent={width,height,1};
        copies.push_back(copy);
        nlohmann::json row={{"level",level},{"width",width},{"height",height},
                            {"offset",size},{"bytes",bytes}};
        if(level<image.info.mip_layout.size()){
            const auto& m=image.info.mip_layout[level];
            row["guest_layout"]={{"offset",m.offset},{"size",m.size},{"pitch",m.pitch},
                {"height",m.height},{"linear_offset",m.linear_offset},{"linear_size",m.linear_size},
                {"linear_pitch",m.linear_pitch},{"tail_x",m.tail_x},{"tail_y",m.tail_y},
                {"surface_z",m.surface_z}};
        }
        layout.push_back(std::move(row));
        size+=bytes;
    }
    auto& download=context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped,offset]=download.Map(size,16u,true);
    if(!mapped){result["status"]="map_failed";return result;}
    // Padding is not image data; return per-mip bytes only, never expose
    // uninitialized alignment padding from the utility buffer.
    download.Commit();
    for(auto& copy:copies)copy.bufferOffset+=offset;
    image.Download(std::span{copies},download.Handle(),offset,size);
    context.GetCommandScheduler().Finish();
    download.Invalidate(offset,size);
    for(auto& row:layout){
        const auto at=row["offset"].get<uint64_t>();
        const auto bytes=row["bytes"].get<uint64_t>();
        std::vector<uint8_t> data(bytes);
        std::memcpy(data.data(),static_cast<const uint8_t*>(mapped)+at,bytes);
        row["native_block_bytes"]=std::move(data);
    }
    result["native_mips"]=std::move(layout);
    result["status"]="captured";
    return result;
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

// Targeted G-buffer preservation proof. Same fixed 8x8 R32_UINT grid as
// DS_LIGHT_BRIDGE, but sampled immediately before and after a material
// draw. This distinguishes a legitimate clear followed by missing geometry
// from a color write that produces black. Nine captures maximum per run.
static std::optional<std::array<uint32_t, 64>> SnapshotDsGBufferGrid(
    RenderContext& context, const RenderColorInfo& color) {
	if (!color.image_id) return std::nullopt;
	auto& image = context.GetTextureCache().GetImage(color.image_id);
	const auto& backing = image.backing;
	const auto& view = color.desc.view_info;
	if (!backing.image || backing.image_type != vk::ImageType::e2D ||
	    backing.samples != 1 || backing.format != vk::Format::eR32Uint ||
	    !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	    view.base_level >= backing.mip_levels || view.base_layer >= backing.layers) return std::nullopt;
	const auto width = std::max(backing.extent.width >> view.base_level, 1u);
	const auto height = std::max(backing.extent.height >> view.base_level, 1u);
	if (width < 16u || height < 16u) return std::nullopt;
	constexpr uint32_t count = 64u;
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(count * 4u, 16u, true);
	if (!mapped) return std::nullopt;
	download.Commit();
	std::array<vk::BufferImageCopy, count> copies {};
	for (uint32_t i = 0; i < count; ++i) {
		auto& c = copies[i];
		c.bufferOffset = offset + uint64_t{i} * 4u;
		c.imageSubresource = {vk::ImageAspectFlagBits::eColor,
		                      view.base_level, view.base_layer, 1};
		c.imageOffset = {static_cast<int32_t>((i % 8u * 2u + 1u) * width / 16u),
		                 static_cast<int32_t>((i / 8u * 2u + 1u) * height / 16u), 0};
		c.imageExtent = {1u, 1u, 1u};
	}
	image.Download(std::span{copies}, download.Handle(), offset, count * 4u);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, count * 4u);
	std::array<uint32_t, count> words {};
	std::memcpy(words.data(), mapped, words.size() * 4u);
	return words;
}

// Continuous pre-light boundary samples, independent of HDR-triggered windows.
// Full population is recorded at each first queue/host-frame boundary;
// exact 64 words accompany every sampled material draw/consumer.
void DemonsPrelightTrace::ObserveCompute(RenderContext& context,const PreparedBindings& bindings,
        uint64_t submit,bool after,bool indirect,uint64_t args_address){
    if(!Enabled()||!bindings.runtime||!bindings.runtime->program)return;
    const auto shader=bindings.runtime->program->shader_hash;
    const auto queue=DemonsCausalMatrix::LightingQueue(shader);
    const auto frame=context.GetGpu().GetFrameNum();
    bool matched=false;
    for(size_t slot=0;slot<bindings.images.size();++slot){
        const auto& b=bindings.images[slot];
        if(!b.image_id||b.placeholder||!Matches(b.desc.info.data.address,b.desc.info.data.size))continue;
        const bool storage=b.desc.type==TextureCache::BindingType::Storage;
        if((after||queue==UINT32_MAX)&&!storage)continue;
        auto& image=context.GetTextureCache().GetImage(b.image_id);
        Watch(image.info.data.address,image.info.data.size);
        const auto view=std::ranges::find(image.views,b.image_view,&CachedImageView::view);
        const bool exact=view!=image.views.end()&&image.info.data.address==DemonsQueueProbe::GBufferAddress&&
            view->info.base_layer==0&&view->info.base_level==0;
        matched|=exact;
        const bool consumer=!after&&queue!=UINT32_MAX&&!storage;
        nlohmann::json row={{"kind",consumer?"consumer":after?"compute_after":"compute_before"},
            {"frame",frame},{"submit",submit},{"tick",context.GetCommandScheduler().CurrentTick()},
            {"shader",shader},{"queue",queue},{"slot",slot},{"storage_binding",storage},
            {"indirect",indirect},{"args_guest",args_address},{"descriptor_guest",b.desc.info.data.address},
            {"exact_layer0",exact},{"image",Identity(image)}};
        if(view!=image.views.end()){
            row["view"]={{"mip",view->info.base_level},{"layer",view->info.base_layer},
                {"layers",view->info.layer_count},{"format",uint32_t(view->info.format)}};
        }
        if(exact&&(consumer||storage)){
            RenderColorInfo color{};color.image_id=b.image_id;color.desc.view_info=view->info;
            if(AllowSample()){
                auto sample=SnapshotDsGBufferGrid(context,color);
                row["grid"]=sample?nlohmann::json(*sample):nlohmann::json();
            }else row["sample_status"]="budget";
            struct RootConsumerSample {
                int32_t frame=-1;uint64_t version=0,native=0;
                nlohmann::json meta;
                std::vector<uint32_t> words;
            };
            static std::array<RootConsumerSample,2> prior{};
            if(consumer&&(queue==0||queue==33)){
                const auto q=queue==0?0u:1u;
                auto& prev=prior[q];
                const auto version=image.ContentVersion();
                const auto native=std::hash<VkImage>{}(static_cast<VkImage>(image.backing.image));
                if((prev.frame!=frame||prev.version!=version||prev.native!=native)&&AllowSample(true)){
                    auto full=SnapshotDsAlbedo(context,color);
                    if(!full.words.empty()){
                        uint64_t hash=14695981039346656037ull,nonzero=0;
                        for(auto v:full.words){nonzero+=DemonsPrelightPolicy::Colored(v);hash=(hash^v)*1099511628211ull;}
                        row["full"]={{"width",full.width},{"height",full.height},
                            {"rgb_nonzero",nonzero},{"hash_words",hash}};
                        auto meta=row;
                        meta["image_after_sample"]=Identity(image);
                        const bool edge=ObservePopulation(frame,queue,nonzero,uint64_t{full.width}*full.height);
                        if(edge){
                            DemonsIndirectWitness::SealAtLoss(frame,queue);
                            std::error_code ec;std::filesystem::create_directories(Folder(),ec);
                            const auto save=[](const char* name,const std::vector<uint32_t>& words){
                                if(words.empty())return false;
                                std::ofstream file(Folder()/name,std::ios::binary|std::ios::trunc);
                                if(!file)return false;
                                file.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(uint32_t));
                                file.close();return file.good();
                            };
                            const bool before_ok=save("ds_prelight_anchor_before.bin",prev.words);
                            const bool after_ok=save("ds_prelight_anchor_after.bin",full.words);
                            std::ofstream out(Folder()/"ds_prelight_anchor.json",std::ios::trunc);
                            if(out)out<<nlohmann::json{{"before",prev.meta},{"after",meta},
                                {"before_saved",before_ok},{"after_saved",after_ok},
                                {"before_file","ds_prelight_anchor_before.bin"},
                                {"after_file","ds_prelight_anchor_after.bin"},
                                {"scope","observed same-queue temporal edge, not expected-output proof"}};
                        }
                        prev={frame,version,native,std::move(meta),std::move(full.words)};
                    }else row["full_status"]="unavailable";
                }
            }
        }
        row["tick_after_sample"]=context.GetCommandScheduler().CurrentTick();
        row["image_after_sample"]=Identity(image);
        Record(std::move(row));
    }
    if(!after&&queue!=UINT32_MAX&&!matched)
        Record({{"kind","consumer_missing_exact_layer0"},{"frame",frame},{"submit",submit},
            {"tick",context.GetCommandScheduler().CurrentTick()},{"shader",shader},{"queue",queue}});
}

// Read the actual D32 depth content used by a G-buffer material's EQUAL
// test at the same coordinates as its R32_UINT output. No test override.
static std::optional<std::array<uint32_t, 64>> SnapshotDsMaterialDepthGrid(
    RenderContext& context, const RenderDepthInfo& depth) {
	if (!depth.image_id) return std::nullopt;
	auto& image = context.GetTextureCache().GetImage(depth.image_id);
	const auto& backing = image.backing;
	const auto& view = depth.desc.view_info;
	if (!backing.image || backing.image_type != vk::ImageType::e2D ||
	    backing.samples != 1 ||
	    (backing.format != vk::Format::eD32Sfloat &&
	     backing.format != vk::Format::eD32SfloatS8Uint) ||
	    !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	    view.base_level >= backing.mip_levels || view.base_layer >= backing.layers)
		return std::nullopt;
	const auto width = std::max(backing.extent.width >> view.base_level, 1u);
	const auto height = std::max(backing.extent.height >> view.base_level, 1u);
	if (width < 16u || height < 16u) return std::nullopt;
	constexpr uint32_t count = 64u;
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(count * sizeof(uint32_t), 16u, true);
	if (!mapped) return std::nullopt;
	download.Commit();
	std::array<vk::BufferImageCopy, count> copies {};
	for (uint32_t i = 0; i < count; ++i) {
		auto& c = copies[i];
		c.bufferOffset = offset + uint64_t{i} * 4u;
		c.imageSubresource = {vk::ImageAspectFlagBits::eDepth,
		                      view.base_level, view.base_layer, 1};
		c.imageOffset = {static_cast<int32_t>((i % 8u * 2u + 1u) * width / 16u),
		                 static_cast<int32_t>((i / 8u * 2u + 1u) * height / 16u), 0};
		c.imageExtent = {1u, 1u, 1u};
	}
	image.Download(std::span{copies}, download.Handle(), offset, count * 4u);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, count * 4u);
	std::array<uint32_t, count> bits {};
	std::memcpy(bits.data(), mapped, bits.size() * 4u);
	return bits;
}

// Depth prepass proof, paired with the existing whole G-buffer capture.
 // A sparse 8x8 grid cannot distinguish a genuinely empty depth frame
 // from missing geometry outside its sample coordinates. One full readback
 // per observed bright/onset/dark phase, no renderer behavior change.
struct DsDepthFullSnapshot {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> bits;
};
static DsDepthFullSnapshot SnapshotDsDepthFull(RenderContext& context,
                                               const RenderDepthInfo& depth) {
    if (!depth.image_id) return {};
    auto& image = context.GetTextureCache().GetImage(depth.image_id);
    const auto& backing = image.backing;
    const auto& view = depth.desc.view_info;
    if (!backing.image || backing.image_type != vk::ImageType::e2D ||
        backing.samples != 1 ||
        (backing.format != vk::Format::eD32Sfloat &&
         backing.format != vk::Format::eD32SfloatS8Uint) ||
        !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
        view.base_level >= backing.mip_levels || view.base_layer >= backing.layers)
        return {};
    const auto width = std::max(backing.extent.width >> view.base_level, 1u);
    const auto height = std::max(backing.extent.height >> view.base_level, 1u);
    const uint64_t area = uint64_t{width} * height;
    if (!area || area > 4ull * 1024u * 1024u) return {};
    const auto bytes = area * sizeof(uint32_t);
    auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped, offset] = download.Map(bytes, 16u, true);
    if (!mapped) return {};
    download.Commit();
    vk::BufferImageCopy copy {};
    copy.bufferOffset = offset;
    copy.imageSubresource = {vk::ImageAspectFlagBits::eDepth,
                             view.base_level, view.base_layer, 1};
    copy.imageExtent = {width, height, 1};
    image.Download(std::span{&copy, 1u}, download.Handle(), offset, bytes);
    context.GetCommandScheduler().Finish();
    download.Invalidate(offset, bytes);
    DsDepthFullSnapshot result {width, height, std::vector<uint32_t>(area)};
    std::memcpy(result.bits.data(), mapped, bytes);
    return result;
}

static void RecordDsMaterialPixels(DemonsQueueProbe::MaterialTrace& trace,
                                   const DsAlbedoSnapshot& before,
                                   const DsAlbedoSnapshot& after, bool cleared) {
	if (before.words.empty() || before.width != after.width || before.height != after.height) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_GBUFFER_PIXELS] frame={} shader=0x{:016x} result=snapshot_unavailable\n",
		    trace.frame, trace.shader));
		return;
	}
	trace.width = before.width;
	trace.height = before.height;
	uint32_t changed = 0, colored_after = 0;
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
	for (size_t i = 0; i < after.words.size(); ++i) {
		colored_after += (after.words[i] & 0x00ffffffu) != 0;
		changed += before.words[i] != after.words[i];
	}
	// This is earlier, real evidence of rendered 3D geometry than the
	// periodic light-queue sample. The 718 capture showed it in the G-buffer
	// while the HDR monitor was bright, before the dark transition. Reuse the
	// material writes rather than guessing a gameplay frame.
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
	    "[DS_GBUFFER_PIXELS] frame={} shader=0x{:016x} "
	    "changed={} cleared_on_load={} colored_after={} extent={}x{} traced={}\n",
	    trace.frame, trace.shader, changed, cleared, colored_after, trace.width, trace.height, trace.count));
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
	if (DemonsPcRenderParity::Enabled()) {
		// U59 preset does not enable KYTY_SKIP_INACTIVE_PS. Mapping is based on
		// backed guest target slots, not on inferred export format or CB_SHADER_MASK.
		for (uint32_t slot=0;slot<RENDER_COLOR_ATTACHMENTS_MAX;++slot) {
			const auto& rt=ctx.GetRenderTarget(slot);
			if(rt.base.addr && rt.info.format!=Prospero::ChannelLayout::kInvalid &&
			    render_target_mask_slot(ctx.GetRenderTargetMask(),slot)) output_mask|=1u<<slot;
		}
		return output_mask;
	}
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
	if(DemonsPcRenderParity::Enabled()) state.ps_active=DemonsPcRenderParity::PixelActive(
	    buffer.GetShaders().GetPs().ps_regs.data_addr!=0,hw.GetRenderTargetMask(),
	    shader_regs.m_cbShaderMask,PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
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
	if(DemonsPcRenderParity::Enabled()) state.ps_active=DemonsPcRenderParity::PixelActive(
	    buffer.GetShaders().GetPs().ps_regs.data_addr!=0,buffer.GetRegisters().GetRenderTargetMask(),
	    shader_regs.m_cbShaderMask,PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
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
	if(!DemonsPcRenderParity::Enabled()) mrt_mask &= color_output_mask;
	// Match the reference PS5 renderer: attachment lifetime follows guest
	// CB_TARGET_MASK, not the subset of PS exports the translator detects.
	// Static/dynamic write masks still honor CB_SHADER_MASK. Even depth-only
	// passes may have a live color attachment that must stay attached.
	static const bool ds_guest_color_targets = [] {
		const char* setting = std::getenv("KYTY_DS_GUEST_COLOR_TARGETS");
		return setting && setting[0] == '1' && setting[1] == '\0';
	}();
	if (ds_guest_color_targets && !DemonsPcRenderParity::Enabled()) {
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
	static const bool isolate = [] {
		const char* value = std::getenv("KYTY_DS_ISOLATE_DEPTH_OCCLUDERS");
		return value && value[0] == '1' && value[1] == '\0';
	}();
	if (isolate && !DemonsPcRenderParity::Enabled() && state.depth_info.image_id && state.ps_active &&
	    state.ps_input_info.stage.program && state.vertex_info[0].stage.program) {
		const auto vs = state.vertex_info[0].stage.program->shader_hash;
		const auto ps = state.ps_input_info.stage.program->shader_hash;
		const bool gbuffer = std::ranges::any_of(std::span{state.color_info, state.color_count},
		    [](const auto& color) { return color.image_id && color.desc.info.data.address == DemonsQueueProbe::GBufferAddress; });
		if (DemonsDominantGeometry::SuppressDepthWrite(isolate, vs, ps,
		    state.depth_info.desc.info.data.address, gbuffer, draw.IsIndexed() && !state.indirect.enabled,
		    state.depth_info.depth_write_enable, draw.index_count, draw.instance_count,
		    static_cast<uint32_t>(state.depth_info.depth_compare_op))) {
			state.depth_info.depth_write_enable = false;
			static std::atomic_uint32_t hits {0};
			const auto n = hits.fetch_add(1, std::memory_order_relaxed);
			if (n < 8 || n % 256 == 0) Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_OCCLUDER_ISOLATE] vs=0x{:016x} ps=0x{:016x} indices={} count={}\n",
			    vs, ps, draw.index_count, n + 1));
		}
	}
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

// Read exactly the GPU index data that the original indexed draw consumes.
static std::vector<uint32_t> ReadDsVertexIndices(RenderContext& context,const PreparedIndexBuffer& source,uint32_t count) {
    if(!source.buffer || !count || count>65536) return {};
    const uint32_t width=source.type==vk::IndexType::eUint16?2u:4u;
    const size_t bytes=size_t{count}*width;
    auto& scheduler=context.GetCommandScheduler();
    auto& download=context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped,dest]=download.Map(bytes,16u,true);
    if(!mapped)return {};
    download.Commit();
    scheduler.EndRendering(PassEnd::Buffer);
    const auto command=scheduler.Current().Handle();
    vk::MemoryBarrier before{};
    before.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
    before.dstAccessMask=vk::AccessFlagBits::eTransferRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
        {},1,&before,0,nullptr,0,nullptr);
    const vk::BufferCopy copy{source.offset,dest,bytes};
    command.copyBuffer(source.buffer,download.Handle(),1,&copy);
    vk::MemoryBarrier after{};
    after.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
    after.dstAccessMask=vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
        {},1,&after,0,nullptr,0,nullptr);
    scheduler.Finish();
    download.Invalidate(dest,bytes);
    std::vector<uint32_t> result(count);
    for(uint32_t i=0;i<count;++i){
        std::memcpy(&result[i],static_cast<const uint8_t*>(mapped)+size_t{i}*width,width);
    }
    return result;
}

std::vector<uint32_t> DemonsPrelightTrace::ReadNativeWords(RenderContext& context,vk::Buffer source,uint64_t offset,uint32_t count){
    return ReadDsVertexIndices(context,PreparedIndexBuffer{source,offset,vk::IndexType::eUint32},count);
}

struct DsNativeSlice {
    vk::Buffer buffer{};
    uint64_t offset=0,bytes=0,skip=0;
};
// One submit/wait for all buffers of a draw. The former per-descriptor
// readback introduced thousands of additional Finish() calls per window.
static std::vector<std::vector<uint8_t>> ReadDsNativeSlices(RenderContext& context,
                                                          std::span<const DsNativeSlice> slices) {
    std::vector<std::vector<uint8_t>> result(slices.size());
    if(slices.empty())return result;
    uint64_t total=0;
    std::vector<uint64_t> offsets;
    for(const auto& slice:slices){
        if(!slice.buffer||!slice.bytes||(slice.offset&3u)||(slice.bytes&3u)||
           slice.skip>=slice.bytes||slice.bytes>8u*1024u*1024u-total)return result;
        offsets.push_back(total);total+=slice.bytes;
    }
    auto& scheduler=context.GetCommandScheduler();
    auto& download=context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped,dest]=download.Map(total,16u,true);
    if(!mapped)return result;
    download.Commit();
    scheduler.EndRendering(PassEnd::Buffer);
    const auto command=scheduler.Current().Handle();
    vk::MemoryBarrier before{};
    before.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
    before.dstAccessMask=vk::AccessFlagBits::eTransferRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
        {},1,&before,0,nullptr,0,nullptr);
    for(size_t i=0;i<slices.size();++i){
        const vk::BufferCopy copy{slices[i].offset,dest+offsets[i],slices[i].bytes};
        command.copyBuffer(slices[i].buffer,download.Handle(),1,&copy);
    }
    vk::MemoryBarrier after{};
    after.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
    after.dstAccessMask=vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
        {},1,&after,0,nullptr,0,nullptr);
    scheduler.Finish();download.Invalidate(dest,total);
    for(size_t i=0;i<slices.size();++i){
        const auto* start=static_cast<const uint8_t*>(mapped)+offsets[i]+slices[i].skip;
        result[i].assign(start,start+slices[i].bytes-slices[i].skip);
    }
    return result;
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
	if (depth.load_clear_aspects) {
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

// PPSA01341: in the dark gameplay capture, D32 covers the entire 1600x900
// target yet the deferred R32_UINT material layer has <2% colored texels.
// Count ALL actual prepared draws reaching that material attachment, including
// the non-scouted shader hashes. This distinguishes missing draw submissions
// from executed draws whose fragment output is lost; no rendering state changes.
static void TraceDsMaterialDrawPopulation(const DrawCallInfo& draw,
                                          const DrawRenderState& state, bool indirect,
                                          int32_t frame) {
	static const bool enabled = [] {
		const auto* env = std::getenv("KYTY_DS_FRAME_TRACE");
		return env && env[0] == '1' && env[1] == '\0';
	}();
	if (!enabled || Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	struct Counts {
		uint32_t draws = 0, indirect = 0, equal = 0;
		uint64_t indices = 0;
	};
	struct Frame {
		int32_t frame = -1;
		const char* phase = nullptr;
		uint32_t depth_draws = 0, depth_only = 0, gbuffer_draws = 0;
		std::unordered_map<uint64_t, Counts> material;
	};
	static Frame current;
	static bool bright_done = false;
	static uint32_t late_count = 0;
	static int32_t last_late_frame = -1;
	if (frame != current.frame) {
		if (current.phase) {
			std::vector<std::pair<uint64_t, Counts>> ranked(
			    current.material.begin(), current.material.end());
			std::sort(ranked.begin(), ranked.end(),
			          [](const auto& a, const auto& b) {
				          return a.second.indices > b.second.indices;
			          });
			std::string summary;
			for (const auto& [hash, count]: ranked) {
				if (summary.size() >= 3000) break;
				summary += fmt::format(" {:016x}:draws={},indices={},indirect={},equal={};",
				    hash, count.draws, count.indices, count.indirect, count.equal);
			}
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_MATERIAL_DRAW_POPULATION] frame={} phase={} depth_draws={} depth_only={} "
			    "gbuffer_draws={} unique_ps={} shader_rows={}\n",
			    current.frame, current.phase, current.depth_draws, current.depth_only,
			    current.gbuffer_draws, current.material.size(), summary));
		}
		current = Frame {};
		current.frame = frame;
		const auto& gate = DemonsAdaptiveCapture::Current();
		if (!bright_done && gate.BrightSceneActive(frame)) {
			current.phase = "bright";
			bright_done = true;
		} else if (gate.PostTransitionDarkDue(frame) && late_count < 3 &&
		           (last_late_frame < 0 || frame - last_late_frame >= 24)) {
			current.phase = "post_transition";
			last_late_frame = frame;
			++late_count;
		}
	}
	if (!current.phase) return;
	const bool depth = state.depth_info.image_id &&
	    state.depth_info.desc.info.data.address == 0x282b60000ull;
	const auto colors = std::span{state.color_info, state.color_count};
	const bool gbuffer = std::ranges::any_of(colors, [](const auto& c) {
		return c.image_id && c.desc.info.data.address == DemonsQueueProbe::GBufferAddress;
	});
	if (depth) {
		++current.depth_draws;
		if (!gbuffer) ++current.depth_only;
	}
	if (!gbuffer) return;
	++current.gbuffer_draws;
	const uint64_t shader = state.ps_active && state.ps_input_info.stage.program
	    ? state.ps_input_info.stage.program->shader_hash : 0ull;
	auto& row = current.material[shader];
	++row.draws;
	row.indices += draw.index_count;
	row.indirect += indirect ? 1u : 0u;
	row.equal += state.depth_info.depth_test_enable &&
	    state.depth_info.depth_compare_op == vk::CompareOp::eEqual;
}


void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable,
	                                     const IndirectDrawArgs* indirect) {
	const PipelineProbe::ScopedRecordPhase record_phase;
    using namespace DemonsPrelightTrace;
    const bool root_trace=DemonsPrelightTrace::Enabled();
    const auto root_frame=m_context.GetGpu().GetFrameNum();
    const auto root_colors=std::span{state.color_info,state.color_count};
    const auto root_target=std::ranges::find_if(root_colors,[](const auto& c){
        return c.image_id && c.desc.info.data.address==DemonsQueueProbe::GBufferAddress &&
               c.desc.view_info.base_layer==0 && c.desc.view_info.base_level==0;
    });
    const bool root_depth=state.depth_info.image_id && state.depth_info.desc.info.data.address==DemonsDominantGeometry::DepthAddress;
    uint64_t root_draw=0;
    const auto* root_vs=state.vertex_info[0].stage.program;
    const auto* root_ps=state.ps_active?state.ps_input_info.stage.program:nullptr;
    const bool root_background_depth=root_depth&&root_vs&&root_ps&&
        root_vs->shader_hash==DemonsDominantGeometry::DominantDepthVs&&
        root_ps->shader_hash==DemonsDominantGeometry::DepthPs;
    const bool root_sky_material=root_target!=root_colors.end()&&root_vs&&root_ps&&
        root_vs->shader_hash==DemonsDominantGeometry::MaterialVs&&
        root_ps->shader_hash==DemonsDominantGeometry::MaterialPs;
    // The #741 witness is one matched prepass/material pair, not W0/W1.
    // Keep only the latest actual operands until this draw removes the color.
    static bool root_witness_latched=false;
    static uint64_t root_witness_pre_serial=0;
    static DsDepthFullSnapshot root_witness_pre_before,root_witness_pre_after;
    bool root_witness_loss=false;
    // Capture before target acquisition; select the cycle on the actual
    // execute event. Never gate on a prepared load-clear flag or a shader.
    struct RootDepthCycle {
        DemonsPrelightPolicy::ExecutedCycle boundary;
        DemonsPrelightPolicy::CycleInputRetention retained;
        std::unordered_map<uint64_t,std::vector<uint8_t>> binary;
        std::unordered_map<uint64_t,std::string> positions;
        uint64_t position_bytes=0,position_dropped=0;
        int32_t frame=-1;
        uint64_t first_submit=0;
    };
    static RootDepthCycle root_cycle;
    bool root_capture_inputs=false;
    std::string root_pending_input;
    std::vector<uint8_t> root_pending_binary;
    std::vector<uint32_t> root_index_words;
    nlohmann::json root_vertex_sources=nlohmann::json::array();
    const int root_phase=root_trace?DemonsPrelightTrace::Phase(root_frame):-1;
    if(root_trace&&command_seq)command_prepared=true;
    if(root_phase>=0&&(root_target!=root_colors.end()||root_depth)){
        const auto* v=state.vertex_info[0].stage.program;
        const auto* p=state.ps_active?state.ps_input_info.stage.program:nullptr;
        root_draw=Record({{"kind","draw_prepared"},{"command",command_seq},{"phase",root_phase},{"frame",root_frame},{"submit",submit_id},
            {"tick",m_context.GetCommandScheduler().CurrentTick()},{"vs",v?v->shader_hash:0ull},
            {"ps",p?p->shader_hash:0ull},{"index_guest",index_source.address},{"index_bytes",index_source.size},
            {"indices",draw.index_count},{"instances",draw.instance_count},{"base_vertex",emit.vertex_offset},
            {"first_instance",emit.first_instance},{"indirect",indirect!=nullptr},
            {"indirect_guest",indirect?indirect->args_addr:0ull},{"topology",uint32_t(topology)},
            {"target_layer0",root_target!=root_colors.end()},{"depth_target",root_depth}});
    }
	auto& ucfg = buffer.GetUserConfig();
	if (CopyDepthToColor(buffer, draw, emit, state)) {
        if(root_draw)Record({{"kind","draw_copy_depth_color"},{"draw",root_draw},{"tick",m_context.GetCommandScheduler().CurrentTick()}});
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
        if(root_draw)Record({{"kind","draw_skipped_pipeline"},{"draw",root_draw},{"tick",m_context.GetCommandScheduler().CurrentTick()}});
		TraceDsGraphicsOutcome(true, false, state.color_count,
		                      static_cast<bool>(state.depth_info.image_id), state.ps_active);
		// --async-shaders: the pipeline is being created on a worker. Nothing recorded so far
		// depends on it (bindings and buffers were only prepared), so the draw is dropped for
		// this frame and drawn once the pipeline is installed.
		Common::PerfStats::CountSkippedDraw();
		return;
	}
	if (SkipDrawForLiveKnobs(state, vertex_stages)) {
        if(root_draw)Record({{"kind","draw_skipped_live_knob"},{"draw",root_draw}});
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
	TraceDsMaterialDrawPopulation(draw, state, indirect != nullptr, ds_gbuffer_frame);
    if(!root_trace && ds_gbuffer_enabled && state.ps_active && bindings.pixel &&
       state.ps_input_info.stage.program && state.ps_input_info.stage.resources &&
       Config::GetShaderDumpFolder().filename()=="ds-lighting-dumps"){
        const auto hash=state.ps_input_info.stage.program->shader_hash;
        const auto slot=DemonsDominantGeometry::DepthProbeSlot(hash);
        const auto& snap=*state.ps_input_info.stage.resources;
        static std::array<uint32_t,DemonsDominantGeometry::DepthProbeShaders.size()> active_captures{};
        if(slot<active_captures.size() && active_captures[slot]<2 && !snap.images.empty() &&
           snap.images.front().dwords[0] && state.depth_info.image_id &&
           state.depth_info.desc.info.data.address==DemonsDominantGeometry::DepthAddress){
            const auto index=active_captures[slot]++;
            const auto evidence=SnapshotDsDiscardImage(m_context,*bindings.pixel,snap.images.front().dwords);
            const auto folder=Config::GetShaderDumpFolder()/"queue-state";
            std::error_code ec;std::filesystem::create_directories(folder,ec);
            const auto name=fmt::format("ds_discard_active_{:016x}_n{}.json",hash,index);
            std::ofstream file(folder/name,std::ios::trunc);
            nlohmann::json samplers=nlohmann::json::array();
            for(const auto& s:snap.samplers)samplers.push_back(s.dwords);
            if(file)file<<nlohmann::json{{"frame",ds_gbuffer_frame},{"submit",submit_id},
                {"tick",m_context.GetCommandScheduler().CurrentTick()},
                {"ps",fmt::format("{:016x}",hash)},
                {"native_ps_file",ShaderDump::ModuleFile(state.programs.pixel.module)},
                {"srt",snap.flattened_srt},{"samplers",std::move(samplers)},
                {"indices",draw.index_count},{"discard_input",evidence}};
            Log::WriteToConsoleAndLog(fmt::format("[DS_DISCARD_ACTIVE] frame={} shader={:016x} file={} saved={}\n",
                ds_gbuffer_frame,hash,name,file.good()));
        }
    }
	DemonsQueueProbe::MaterialTrace* ds_material_trace = nullptr;
	DsAlbedoSnapshot ds_albedo_before;
	DsAlbedoSnapshot ds_live_material_full_before {};
	bool ds_live_material_capture = false;
	int ds_live_material_phase = -1;
	RenderColorInfo ds_live_material_target {};
	RenderDepthInfo ds_live_material_depth {};
	std::optional<std::array<uint32_t, 64>> ds_live_material_before {};
	std::optional<std::array<uint32_t, 64>> ds_live_depth_before {};
	// The #645 onset capture proved the material G-buffer is black while
	// the paired D32 depth contains 64 zero samples *before* the material
	// draw. Capture the actual D32 prepass writer instead of adding another
	// compositor or light-atlas probe. Exactly one draw in each observed
	// bright/onset/dark phase; no absolute frame number is assumed.
	std::optional<std::array<uint32_t, 64>> ds_depth_prepass_before {};
	DsDepthFullSnapshot ds_depth_full_before {};
	DsDepthFullSnapshot ds_material_depth_full_before {};
	RenderDepthInfo ds_depth_prepass_target {};
	int ds_depth_prepass_phase = -1;
	if (Config::GetShaderDumpFolder().filename() == "ds-lighting-dumps" &&
	    state.depth_info.image_id && state.depth_info.depth_write_enable &&
	    state.depth_info.desc.info.data.address == 0x282b60000ull) {
		const auto& gate = DemonsAdaptiveCapture::Current();
		const int phase = gate.PostTransitionDarkDue(ds_gbuffer_frame) ? 3
		    : gate.DarkOnsetDue(ds_gbuffer_frame) ? 1
		    : gate.BrightBaselineDue(ds_gbuffer_frame) ? 0
		    : gate.CaptureDue(ds_gbuffer_frame) ? 2 : -1;
		static std::array<bool, 4> captured {};
		// The first draw after HTile consumption often writes no samples.
		// Capture the actual subsequent depth owners in the same phase as well.
		// Onset/post-transition: two large occluder draws, the full-screen
		// depth/material pair and the two rejected scenery depth draws.
		static std::array<std::array<bool, 5>, 4> owner_captured {};
		const auto* depth_vs = state.vertex_info[0].stage.program;
		const auto* depth_ps = state.ps_active ? state.ps_input_info.stage.program : nullptr;
		int owner = -1;
		if (depth_vs && draw.IsIndexed() && !indirect) {
			if (depth_vs->shader_hash == 0x2337192397a30339ull) {
				if (draw.index_count == 7296) owner = 0;
				else if (draw.index_count == 192) owner = 1;
			} else if (depth_vs->shader_hash == 0xaff9c7ef505c6253ull) {
				owner = 2;
			} else if (depth_ps && depth_ps->shader_hash == 0x3dce2bd78d58db62ull) {
				owner = 3;
			} else if (depth_ps && depth_ps->shader_hash == 0x895c9fe037e704f5ull) {
				owner = 4;
			}
		}
		const bool owner_due = (phase == 1 || phase == 3) && owner >= 0 &&
		    !owner_captured[phase][owner];
		if (phase >= 0 && (!captured[phase] || owner_due)) {
			captured[phase] = true;
			if (owner_due) owner_captured[phase][owner] = true;
			ds_depth_prepass_phase = phase;
			ds_depth_prepass_target = state.depth_info;
			ds_depth_prepass_before = SnapshotDsMaterialDepthGrid(m_context, ds_depth_prepass_target);
			ds_depth_full_before = SnapshotDsDepthFull(m_context, ds_depth_prepass_target);
		}
	}
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
	// The 765 comparison established full D32 coverage with a mostly
	// 0xFF000000 first material layer while other material layers remained
	// populated. Probe the two unmeasured high-index PS hashes alongside
	// the three known shaders in the observed post-transition phase.
	// Do not assume every nonzero auxiliary layer implies a valid material.
	if (state.ps_active && state.ps_input_info.stage.program &&
	    Config::GetShaderDumpFolder().filename() == "ds-lighting-dumps") {
		const auto shader = state.ps_input_info.stage.program->shader_hash;
		const auto& watched = DemonsQueueProbe::GBufferShaders;
		const auto match = std::ranges::find(watched, shader);
		if (match != watched.end()) {
			const auto colors = std::span{state.color_info, state.color_count};
			const auto target = std::ranges::find_if(colors, [](const auto& color) {
				return color.image_id && color.desc.info.data.address == DemonsQueueProbe::GBufferAddress &&
				       color.desc.view_info.base_layer == 0 && color.desc.view_info.base_level == 0;
			});
			if (target != colors.end()) {
				const auto& gate = DemonsAdaptiveCapture::Current();
				static std::array<int32_t, 4> phase_frame {-1, -1, -1, -1};
				static std::array<std::array<bool, DemonsQueueProbe::GBufferShaders.size()>, 4> phase_shader_done {};
				const int phase = gate.PostTransitionDarkDue(ds_gbuffer_frame) ? 3
				    : gate.DarkOnsetDue(ds_gbuffer_frame) ? 1
				    : gate.BrightBaselineDue(ds_gbuffer_frame) ? 0
				    : gate.CaptureDue(ds_gbuffer_frame) ? 2 : -1;
				if (phase >= 0 && (phase == 3 || static_cast<size_t>(match - watched.begin()) < 3)) {
					if (phase_frame[phase] < 0) phase_frame[phase] = ds_gbuffer_frame;
					const auto shader_index = static_cast<size_t>(match - watched.begin());
					if (phase_frame[phase] == ds_gbuffer_frame &&
					    !phase_shader_done[phase][shader_index]) {
						phase_shader_done[phase][shader_index] = true;
						ds_live_material_target = *target;
						ds_live_material_depth = state.depth_info;
						ds_live_material_phase = phase;
						if (shader == DemonsDominantGeometry::MaterialPs) {
							const auto encode = [](const auto& stage) {
								nlohmann::json out = nlohmann::json::object();
								if (!stage.program || !stage.resources) return out;
								const auto& data = *stage.resources;
								out["hash"] = fmt::format("{:016x}", stage.program->shader_hash);
								out["user_data"] = data.user_data;
								out["srt"] = data.flattened_srt;
								out["buffers"] = nlohmann::json::array();
								for (const auto& desc: data.buffers) out["buffers"].push_back(desc.dwords);
								out["images"] = nlohmann::json::array();
								for (const auto& desc: data.images) out["images"].push_back(desc.dwords);
								out["samplers"] = nlohmann::json::array();
								for (const auto& desc: data.samplers) out["samplers"].push_back(desc.dwords);
								return out;
							};
							const auto folder = Config::GetShaderDumpFolder() / "queue-state";
							std::error_code ec;
							std::filesystem::create_directories(folder, ec);
							const auto name = fmt::format("ds_dominant_material_f{}_s{}.json", ds_gbuffer_frame, submit_id);
							std::ofstream file(folder / name, std::ios::trunc);
							if (file) file << nlohmann::json{
							    {"frame", ds_gbuffer_frame}, {"submit", submit_id},
							    {"vertex", encode(state.vertex_info[0].stage)},
							    {"pixel", encode(state.ps_input_info.stage)},
							    {"output_mode", state.ps_input_info.target_output_mode},
							    {"target_uint_mask", state.ps_input_info.target_uint_mask},
							    {"target_sint_mask", state.ps_input_info.target_sint_mask},
							    {"target_shader_mask", state.ps_input_info.target_shader_mask}};
						}
						ds_live_material_before = SnapshotDsGBufferGrid(
						    m_context, ds_live_material_target);
						ds_live_depth_before = SnapshotDsMaterialDepthGrid(
						    m_context, ds_live_material_depth);
						ds_live_material_capture = ds_live_material_before.has_value();
						if(ds_live_material_capture)
							ds_live_material_full_before = SnapshotDsAlbedo(
							    m_context, ds_live_material_target);
						// The first low-coverage material pass needs one
						// whole-D32 observation after the earlier HTile
						// clear, not another 64-point grid.
						ds_material_depth_full_before = SnapshotDsDepthFull(
						    m_context, ds_live_material_depth);
					}
				}
			}
		}
	}
    // Take the before image before AcquireRenderTargets materializes loadOp.
    // Every prepared target draw gets an explicit sample or missing marker.
    RenderColorInfo root_color{};
    std::optional<std::array<uint32_t,64>> root_before;
    nlohmann::json root_identity,root_full_before;
    const auto root_population=[&](const RenderColorInfo& color){
        nlohmann::json data;
        const auto snap=SnapshotDsAlbedo(m_context,color);
        if(!snap.words.empty()){
            uint64_t colored=0,hash=14695981039346656037ull;
            for(const auto w:snap.words){colored+=DemonsPrelightPolicy::Colored(w);hash=(hash^w)*1099511628211ull;}
            data={{"rgb_nonzero",colored},{"hash_words",hash},{"width",snap.width},{"height",snap.height}};
        }
        return data;
    };
    if(root_draw){
        root_capture_inputs=root_trace&&!root_witness_latched&&
            (root_depth||root_target!=root_colors.end());
        nlohmann::json inputs={{"kind","background_inputs"},{"draw",root_draw},{"frame",root_frame},
            {"submit",submit_id},{"tick",m_context.GetCommandScheduler().CurrentTick()},
            {"vs_hash",fmt::format("{:016x}",root_vs?root_vs->shader_hash:0ull)},
            {"ps_hash",fmt::format("{:016x}",root_ps?root_ps->shader_hash:0ull)},
            {"vs_file",ShaderDump::ModuleFile(state.programs.vertex[0].module)},
            {"ps_file",ShaderDump::ModuleFile(state.programs.pixel.module)},
            {"vs_original_file",root_vs&&root_vs->ds_light_math_variant?
                fmt::format("spv/vs_{:016x}_{:016x}.spv",root_vs->shader_hash,root_vs->ds_light_math_variant):std::string{}},
            {"vs_float_mode",buffer.GetShaders().GetVs().gs_regs.rsrc1.float_mode},
            {"vs_ieee_mode",buffer.GetShaders().GetVs().gs_regs.rsrc1.ieee_mode},
            {"vs_dx10_clamp",buffer.GetShaders().GetVs().gs_regs.rsrc1.dx10_clamp},
            {"vs_float_mode_source","SPI_SHADER_PGM_RSRC1_GS"},
            {"base_vertex",emit.vertex_offset},{"first_vertex",emit.first_vertex},{"first_instance",emit.first_instance},
            {"instances",draw.instance_count},{"index_guest",index_source.address},
            {"index_element_bytes",index_source.guest_element_size},
            {"depth_load_clear",state.depth_info.depth_load_clear_enable},
            {"depth_clear_value",state.depth_info.depth_clear_value}};
        if(state.depth_info.image_id){
            inputs["depth_image"]=Identity(m_context.GetTextureCache().GetImage(state.depth_info.image_id));
            if(!state.depth_info.depth_load_clear_enable&&AllowSample()){
                const auto d=SnapshotDsMaterialDepthGrid(m_context,state.depth_info);
                inputs["depth_grid"]=d?nlohmann::json(*d):nlohmann::json();
            }
        }
        if(state.ps_input_info.stage.resources){
            const auto& snap=*state.ps_input_info.stage.resources;
            inputs["ps_srt"]=snap.flattened_srt;
            inputs["ps_user_data"]=snap.user_data;
            inputs["ps_buffers"]=nlohmann::json::array();
            for(const auto& b:snap.buffers)inputs["ps_buffers"].push_back(b.dwords);
        }
        if(state.vertex_info[0].stage.resources){
            const auto& snap=*state.vertex_info[0].stage.resources;
            inputs["vs_srt"]=snap.flattened_srt;
            inputs["vs_user_data"]=snap.user_data;
            inputs["vs_buffers"]=nlohmann::json::array();
            for(const auto& b:snap.buffers)inputs["vs_buffers"].push_back(b.dwords);
        }
        const auto scalar_reads=[&](const char* prefix,const auto* resources){
            if(!root_capture_inputs||!resources)return;
            const auto& snap=*resources;
            auto regions=nlohmann::json::array();
            for(const auto& read:snap.diagnostic_scalar_reads){
                regions.push_back({{"address",read.address},{"bytes",read.words.size()*4u},
                    {"source","consumed_srt"},{"read_kind",read.kind==0?"host":read.kind==1?"specialization":read.kind==4?"ordinary":"branch"},
                    {"words",read.words}});
            }
            inputs[std::string(prefix)+"_scalar_regions"]=std::move(regions);
            inputs[std::string(prefix)+"_scalar_capture_complete"]=snap.diagnostic_scalar_complete;
            inputs[std::string(prefix)+"_user_data_base"]=snap.diagnostic_user_data_base;
        };
        scalar_reads("vs",state.vertex_info[0].stage.resources);
        scalar_reads("ps",state.ps_input_info.stage.resources);
        inputs["kind"]="draw_inputs";
        inputs["native_index_count"]=draw.index_count;
        if(draw.IsIndexed()&&!mesh_active&&!indirect&&draw.index_count&&draw.index_count<=65536&&
           index_source.guest_element_size&&uint64_t{draw.index_count}*index_source.guest_element_size<=index_source.size){
            auto raw=ReadDsVertexIndices(m_context,index_binding,draw.index_count);
            inputs["index_words_captured"]=raw.size();
            inputs["index_digest"]=DemonsPrelightTrace::VertexWordDigest(raw);
            // Retain complete short index arrays; longer streams are not certified by digest alone.
            if(raw.size()<=4096||root_capture_inputs)inputs["index_words"]=raw;
            if(root_capture_inputs)root_index_words=std::move(raw);
        }
        if(root_capture_inputs&&!draw.IsIndexed()&&!indirect&&!mesh_active&&draw.index_count<=65536u){
            root_index_words.resize(draw.index_count);
            for(uint32_t i=0;i<draw.index_count;++i)root_index_words[i]=i;
            inputs["index_words"]=root_index_words;
            inputs["index_words_synthetic"]=true;
        }
        std::vector<DsNativeSlice> slices;
        std::vector<std::pair<std::string,size_t>> slice_slots;
        uint64_t batch_bytes=0;
        const auto encode_bound=[&](const PreparedBindings& bound,const auto* program,const char* key){
            auto out=nlohmann::json::array();
            if(!program)return out;
            for(uint32_t slot=0;slot<bound.buffer_sources.size()&&slot<bound.buffers.size();++slot){
                const auto& src=bound.buffer_sources[slot];const auto& native=bound.buffers[slot];
                nlohmann::json item={{"slot",slot},{"address",src.address},{"bytes",src.size}};
                uint64_t folded=0;
                const bool valid=DemonsPrelightPolicy::DiagnosticRange(src.address,src.size)&&
                    Kernel::AddressFold::FoldSpan(src.address,src.size,&folded)&&
                    DemonsPrelightPolicy::DiagnosticRange(folded,src.size);
                if(!valid){
                    // A placeholder is not evidence of clean memory, nor an error in gameplay.
                    item["gpu_dirty"]=nullptr;
                    item["capture_status"]="empty_or_invalid_guest_range";
                    out.push_back(std::move(item));continue;
                }
                item["gpu_dirty"]=m_context.GetBufferCache().HasGpuDirtyBytes(src.address,src.size);
                const auto word=program->bindings.memory_offset_dword+slot/4u;
                if(word<bound.shader_data.size()&&native.buffer&&
                   src.size<=(root_capture_inputs?4u*1024u*1024u:1024u)){
                    const uint64_t adjust=(bound.shader_data[word]>>((slot%4u)*8u))&255u;
                    if(DemonsPrelightPolicy::DiagnosticSlice(native.offset,native.range,adjust,src.size)){
                        const auto start=native.offset+adjust;
                        const auto skip=start&3u;
                        const auto aligned=start-skip;
                        const auto length=(src.size+skip+3u)&~uint64_t{3};
                        if(aligned<native.offset||length>native.range-(aligned-native.offset)){
                            item["capture_status"]="unaligned_slice_exceeds_native_range";
                        }else if(length>8u*1024u*1024u-batch_bytes){
                            item["capture_status"]="draw_readback_budget_exceeded";
                        }else{
                            slices.push_back({native.buffer,aligned,length,skip});
                            slice_slots.emplace_back(key,out.size());batch_bytes+=length;
                            item["capture_status"]="pending_batch";
                            item["native_offset"]=start;item["native_range"]=native.range;
                        }
                    }else item["capture_status"]="invalid_native_slice";
                }else item["capture_status"]="descriptor_or_capture_size_unsupported";
                out.push_back(std::move(item));
            }
            return out;
        };
        inputs["vs_bound"]=encode_bound(bindings.vertex[0],root_vs,"vs_bound");
        if(bindings.pixel&&root_ps)inputs["ps_bound"]=encode_bound(*bindings.pixel,root_ps,"ps_bound");
        const auto captured=ReadDsNativeSlices(m_context,slices);
        for(size_t i=0;i<slice_slots.size();++i){
            auto& item=inputs[slice_slots[i].first][slice_slots[i].second];
            const auto size=item["bytes"].get<size_t>();
            if(captured[i].size()<size){item["capture_status"]="readback_unavailable";continue;}
            item["capture_status"]="complete";
            if(!root_capture_inputs||(size<=1024u&&(size%4u)==0)){
                std::vector<uint32_t> words(size/4u);
                std::memcpy(words.data(),captured[i].data(),words.size()*4u);
                item["gpu_words"]=std::move(words);
            }else{
                while(root_pending_binary.size()%4u)root_pending_binary.push_back(0);
                item["file"]=fmt::format("queue-state/ds_prelight_witness_data_{}.bin",root_draw);
                item["offset_bytes"]=root_pending_binary.size();
                root_pending_binary.insert(root_pending_binary.end(),captured[i].begin(),captured[i].begin()+size);
            }
        }
        if(root_capture_inputs)root_vertex_sources=inputs["vs_bound"];
        if(root_capture_inputs){
            inputs["cycle_first_draw_before_acquire"]=root_cycle.boundary.first;
            inputs["indexed"]=draw.IsIndexed();
            inputs["indirect"]=indirect!=nullptr;
            inputs["mesh"]=mesh_active;
            inputs["vs_shader_data"]=bindings.vertex[0].shader_data;
            if(bindings.pixel&&root_ps)inputs["ps_shader_data"]=bindings.pixel->shader_data;
            if(state.ps_input_info.stage.resources){
                const auto& snap=*state.ps_input_info.stage.resources;
                inputs["ps_images"]=nlohmann::json::array();
                inputs["ps_samplers"]=nlohmann::json::array();
                for(const auto& v:snap.images)inputs["ps_images"].push_back(v.dwords);
                for(const auto& v:snap.samplers)inputs["ps_samplers"].push_back(v.dwords);
            }
            // Admission happens after acquisition exposes the actual clear.
            // The previous cycle may already be full; it must not consume
            // the budget of this pending first record of the next cycle.
            root_pending_input=inputs.dump();
            inputs["witness_input_pending"]=true;
            // Keep the regular root journal small. The exact complete
            // bytes above are retained separately for the matched cycle.
            for(const char* key:{"vs_bound","ps_bound"}){
                if(!inputs.contains(key))continue;
                for(auto& entry:inputs[key]){
                    if(entry.contains("gpu_words")&&entry["gpu_words"].size()>256){
                        const auto words=entry["gpu_words"].get<std::vector<uint32_t>>();
                        entry["gpu_digest"]=DemonsPrelightTrace::VertexWordDigest(words);
                        entry.erase("gpu_words");
                    }
                }
            }
            if(inputs.contains("index_words")&&inputs["index_words"].size()>4096)inputs.erase("index_words");
            inputs.erase("vs_shader_data");inputs.erase("ps_shader_data");
            inputs.erase("vs_scalar_regions");inputs.erase("ps_scalar_regions");
        }
        Record(std::move(inputs));
    }
    if(root_draw && root_target!=root_colors.end()){
        root_color=*root_target;
        auto& im=m_context.GetTextureCache().GetImage(root_color.image_id);
        Watch(im.info.data.address,im.info.data.size);
        root_identity=Identity(im);
        if(AllowSample()){
            root_before=SnapshotDsGBufferGrid(m_context,root_color);
            root_full_before=root_population(root_color);
        }
        Record({{"kind","draw_before"},{"draw",root_draw},{"frame",root_frame},{"submit",submit_id},
            {"tick",m_context.GetCommandScheduler().CurrentTick()},{"image_before_sample",root_identity},
            {"image",Identity(im)},{"grid",root_before?nlohmann::json(*root_before):nlohmann::json()},
            {"full",root_full_before},{"mip",root_color.desc.view_info.base_level},{"layer",root_color.desc.view_info.base_layer},
            {"slot",root_color.target_slot}});
    }
    // Causal material census: every layer-0 attachment draw, not only
    // the previously watched hashes. Record the actual before/after bytes
    // on a bounded two-host-frame window (guest flips can split the pass).
    static std::array<int32_t,4> chain_start{-1,-1,-1,-1};
    static std::array<nlohmann::json,4> chain_rows{};
    constexpr size_t chain_limit=96;
    int chain_phase=-1;
    RenderColorInfo chain_target{};
    DsAlbedoSnapshot chain_before{};
    std::optional<std::array<uint32_t,64>> chain_depth{};
    uint64_t chain_version_before=0;
    if(!root_trace && ds_gbuffer_enabled && Config::GetShaderDumpFolder().filename()=="ds-lighting-dumps"){
        const auto colors=std::span{state.color_info,state.color_count};
        const auto target=std::ranges::find_if(colors,[](const auto& c){
            return c.image_id && c.desc.info.data.address==DemonsQueueProbe::GBufferAddress &&
                c.desc.view_info.base_layer==0 && c.desc.view_info.base_level==0;
        });
        const auto& gate=DemonsAdaptiveCapture::Current();
        const int phase=gate.PostTransitionDarkDue(ds_gbuffer_frame)?3:
            gate.DarkOnsetDue(ds_gbuffer_frame)?1:gate.BrightBaselineDue(ds_gbuffer_frame)?0:
            gate.CaptureDue(ds_gbuffer_frame)?2:-1;
        if(target!=colors.end()){
            if(phase>=0 && chain_start[phase]<0){
                chain_start[phase]=ds_gbuffer_frame;
                chain_rows[phase]=nlohmann::json::array();
            }
            for(int p=3;p>=0;--p){
                if(chain_start[p]>=0 && ds_gbuffer_frame>=chain_start[p] &&
                   ds_gbuffer_frame<=chain_start[p]+1 && chain_rows[p].size()<chain_limit){
                    chain_phase=p;chain_target=*target;
                    chain_version_before=m_context.GetTextureCache().GetImage(target->image_id).ContentVersion();
                    chain_before=SnapshotDsAlbedo(m_context,chain_target);
                    chain_depth=SnapshotDsMaterialDepthGrid(m_context,state.depth_info);
                    break;
                }
            }
        }
    }
	// Follow the first post-compute HDR writer and reader, then the compositor.
	constexpr uint8_t DsHdrReader = 1, DsComposite = 2, DsHdrWriter = 4;
	std::array<uint8_t, DemonsQueueProbe::Queues.size()> ds_draw_stage {};
	bool ds_live_composite_output = false;
	bool ds_scene_trace = false;
	bool ds_scene_bright = false, ds_scene_onset = false, ds_scene_paired = false;
	bool ds_scene_post_transition = false;
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
		// The recovered 720 samples were twelve small additive overlay
		// draws. The frequent 0x17defa... shader writes the SAME scene
		// target in both the illuminated and dark phases, but was excluded.
		// Limit readbacks to that verified writer, in three observed phases.
		static int32_t scene_frame = -1;
		static uint32_t scene_ordinal = 0;
		static int32_t bright_baseline_frame = -1;
		static int32_t dark_onset_frame = -1;
		static int32_t paired_dark_frame = -1;
		static int32_t active_late_scene_frame = -1;
		static int32_t last_late_scene_frame = -1;
		static uint32_t late_scene_count = 0;
		if (scene_frame != frame) { scene_frame = frame; scene_ordinal = 0; }
		const auto& gate = DemonsAdaptiveCapture::Current();
		const auto colors = std::span {state.color_info, state.color_count};
		const auto scene = std::ranges::find_if(colors, [](const auto& color) {
			return color.image_id && color.desc.info.data.address == DemonsQueueProbe::SceneAddress;
		});
		const bool core_writer = shader == 0x17defa333d9d1e22ull;
		if (scene != colors.end() && core_writer) {
			if (bright_baseline_frame < 0 && gate.BrightBaselineDue(frame))
				bright_baseline_frame = frame;
			if (dark_onset_frame < 0 && gate.DarkOnsetDue(frame))
				dark_onset_frame = frame;
			if (paired_dark_frame < 0 && gate.CapturedFrame(frame) &&
			    !gate.PostTransitionDarkDue(frame))
				paired_dark_frame = frame;
			// Scene writer draws may precede the coefficient/atlas
			// capture in the same frame. Record up to three distinct
			// post-transition scene frames without waiting for pairing.
			if (gate.PostTransitionDarkDue(frame) &&
			    active_late_scene_frame != frame && late_scene_count < 3 &&
			    (last_late_scene_frame < 0 || frame - last_late_scene_frame >= 24)) {
				active_late_scene_frame = frame;
				last_late_scene_frame = frame;
				++late_scene_count;
			}
		}
		const bool late_paired_frame = frame == active_late_scene_frame;
		const bool relevant_frame = frame == bright_baseline_frame ||
		                            frame == dark_onset_frame ||
		                            frame == paired_dark_frame ||
		                            late_paired_frame;
		if (scene != colors.end() && core_writer && relevant_frame &&
		    Config::GetShaderDumpFolder().filename() == "ds-lighting-dumps") {
			const uint32_t ordinal = ++scene_ordinal;
			// Bounded 64-pixel snapshots across the actual writer sequence,
			// rather than sampling the first twelve small overlays.
			ds_scene_trace = ordinal == 1u || ordinal == 8u || ordinal == 32u ||
			                 ordinal == 64u || ordinal == 128u || ordinal == 256u;
		}
		if (ds_scene_trace) {
			ds_scene_bright = frame == bright_baseline_frame;
			ds_scene_onset = frame == dark_onset_frame;
			ds_scene_paired = frame == paired_dark_frame;
			ds_scene_post_transition = late_paired_frame;
			ds_scene_pass = scene_ordinal;
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
    DemonsPrelightTrace::active_draw=root_draw;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);
    DemonsPrelightTrace::active_draw=0;

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
    namespace VP=DemonsVertexPositionProbe;
    std::vector<uint32_t> ds_vertex_indices;
    uint32_t ds_vertex_base=0,ds_vertex_count=0;
    int ds_vertex_phase=-1;
    uint64_t ds_vertex_pair=0;
    const int64_t ds_vertex_offset=draw.IsIndexed()?int64_t{emit.vertex_offset}:int64_t{emit.first_vertex};
    bool ds_vertex_sky_pair=false;
    const char* ds_vertex_status="native_vertex_instrumentation_unavailable";
    DsDepthFullSnapshot ds_vertex_depth_before;
    nlohmann::json ds_vertex_sources=nlohmann::json::array();
    nlohmann::json ds_vertex_pixel=nlohmann::json::object();
    struct DsVertexPair {
        int32_t frame=-1;
        int phase=-1;
        uint64_t serial=0,address=0;
        uint32_t count=0,instance=0;
        int32_t base=0;
        bool material_captured=false;
    };
    static std::array<DsVertexPair,VP::FocusPairCount> ds_vertex_pairs{};
    static uint64_t ds_vertex_serial=0;
    const auto* ds_vp=state.vertex_info[0].stage.program;
    if(ds_vp && (root_trace||VP::Watched(ds_vp->shader_hash)) && ds_vp->ds_light_math_variant){
        const auto& gate=DemonsAdaptiveCapture::Current();
        const auto* ps=state.ps_active?state.ps_input_info.stage.program:nullptr;
        const int phase=root_trace?std::max(root_phase,0):gate.PostTransitionDarkDue(ds_gbuffer_frame)?3:
            gate.DarkOnsetDue(ds_gbuffer_frame)?2:gate.BrightSceneActive(ds_gbuffer_frame)?1:0;
        static std::array<std::array<uint32_t,4>,VP::FocusPairCount> counts{};
        const uint32_t k=VP::FocusPair(ds_vp->shader_hash,ps?ps->shader_hash:0,draw.index_count);
        const bool material=VP::Material(ds_vp->shader_hash);
        const bool matched=k<VP::FocusPairCount;
        // Invalid category still binds disabled recorder, never indexes
        // outside the selected pair array.
        auto& pair=ds_vertex_pairs[matched?k:0];
        // Both persistent zero-cycle meshes were observed in prepass and
        // material (#729 frames 249/250). Select them by exact index count
        // AND pair identity, not the first occurrence of a shader.
        const bool root_witness=root_trace&&root_phase>=0&&k==4&&
            !root_witness_latched&&draw.index_count>0&&draw.index_count<=64;
        const bool legacy_due=(!root_trace||root_witness) && matched && (material ?
            pair.serial && !pair.material_captured && ds_gbuffer_frame>=pair.frame &&
            int64_t{ds_gbuffer_frame}-pair.frame<=1 &&
            pair.address==index_source.address && pair.count==draw.index_count &&
            pair.base==emit.vertex_offset && pair.instance==emit.first_instance :
            (root_trace?root_witness:(phase>=2&&counts[k][phase]<1u)));
        const bool due=root_trace?root_capture_inputs:legacy_due;
        ds_vertex_status="unsupported_draw_shape";
        if(due && vertex_stages.size()==1 && !mesh_active && !mesh_emulated && !indirect &&
            (draw.IsIndexed()||root_trace) && draw.instance_count==1 && !primitive_restart_enable &&
            topology==vk::PrimitiveTopology::eTriangleList &&
            (!draw.IsIndexed()||index_source.guest_element_size>=2) &&
            (root_trace||(state.depth_info.image_id && state.depth_info.desc.info.data.address==DemonsDominantGeometry::DepthAddress))){
            ds_vertex_indices=root_trace?root_index_words:ReadDsVertexIndices(m_context,index_binding,draw.index_count);
            ds_vertex_status="index_readback_unavailable";
            if(!ds_vertex_indices.empty()){
                const auto [lo,hi]=std::minmax_element(ds_vertex_indices.begin(),ds_vertex_indices.end());
                const int64_t base=int64_t{*lo}+ds_vertex_offset, end=int64_t{*hi}+ds_vertex_offset;
                ds_vertex_status="vertex_span_exceeds_recorder";
                if(base>=0 && end<=UINT32_MAX && end-base<VP::MaxVertices){
                    ds_vertex_base=static_cast<uint32_t>(base);
                    ds_vertex_count=static_cast<uint32_t>(end-base+1);
                    ds_vertex_status="armed";
                    if(root_trace&&k!=4){
                        ds_vertex_phase=phase;ds_vertex_pair=root_draw;
                    }else if(material&&pair.serial&&pair.address==index_source.address&&
                             pair.count==draw.index_count&&pair.base==emit.vertex_offset&&
                             pair.instance==emit.first_instance&&ds_gbuffer_frame>=pair.frame&&
                             int64_t{ds_gbuffer_frame}-pair.frame<=1){
                        ds_vertex_phase=pair.phase;
                        ds_vertex_pair=pair.serial;
                        pair.material_captured=true;
                        ds_vertex_sky_pair=root_trace&&k==4;
                    }else{
                        ds_vertex_phase=phase;
                        ds_vertex_pair=++ds_vertex_serial;
                        pair={ds_gbuffer_frame,phase,ds_vertex_pair,index_source.address,
                            draw.index_count,emit.first_instance,emit.vertex_offset,false};
                        if(!root_trace)++counts[k][phase];
                        ds_vertex_sky_pair=root_trace&&k==4&&!material;
                    }
                    // This read is before the current draw, including before
                    // any pending attachment load clear. Record that clear
                    // separately; do not compare the pre-clear image as if
                    // it were the depth tested by the draw.
                    if(!root_trace||ds_vertex_sky_pair)
                        ds_vertex_depth_before=SnapshotDsDepthFull(m_context,state.depth_info);
                    // Record the actual descriptors used by the dominant
                    // position shader, not a later CPU reconstruction.
                    const auto& bound=bindings.vertex[0];
                    if(root_trace)ds_vertex_sources=root_vertex_sources;
                    for(uint32_t slot=0;!root_trace&&slot<bound.buffer_sources.size() && slot<bound.buffers.size();++slot){
                        const auto& source=bound.buffer_sources[slot];
                        const auto& native=bound.buffers[slot];
                        nlohmann::json entry={{"slot",slot},{"address",source.address},{"bytes",source.size}};
                        const auto word=ds_vp->bindings.memory_offset_dword+slot/4u;
                        uint64_t folded=0;
                        const bool valid_source=DemonsPrelightPolicy::DiagnosticRange(source.address,source.size)&&
                            Kernel::AddressFold::FoldSpan(source.address,source.size,&folded)&&
                            DemonsPrelightPolicy::DiagnosticRange(folded,source.size);
                        if(valid_source&&word<bound.shader_data.size() && native.buffer &&
                           source.size<=262144u){
                            const uint64_t adjust=(bound.shader_data[word]>>((slot%4u)*8u))&255u;
                            if(DemonsPrelightPolicy::DiagnosticSlice(native.offset,native.range,adjust,source.size)){
                                auto words=ReadDsVertexIndices(m_context,
                                    PreparedIndexBuffer{native.buffer,native.offset+adjust,vk::IndexType::eUint32},
                                    static_cast<uint32_t>(source.size/4));
                                entry["gpu_words"]=std::move(words);
                                std::vector<uint32_t> backing(source.size/4);
                                if(LibKernel::Memory::TryReadBacking(source.address,backing.data(),source.size))
                                    entry["backing_words"]=std::move(backing);
                                entry["gpu_dirty"]=m_context.GetBufferCache().HasGpuDirtyBytes(source.address,source.size);
                                entry["cpu_modified"]=m_context.GetBufferCache().IsRegionCpuModified(source.address,source.size);
                            }
                        }
                        ds_vertex_sources.push_back(std::move(entry));
                    }
                    if(state.ps_active && state.ps_input_info.stage.resources){
                        const auto& ps_snap=*state.ps_input_info.stage.resources;
                        ds_vertex_pixel={{"srt",ps_snap.flattened_srt},{"user_data",ps_snap.user_data}};
                    }
                }
            }
        }
        const bool capture=ds_vertex_phase>=0;
        auto& storage=m_ds_vertex_position_buffers[capture];
        const bool init=!storage;
        const auto bytes=(capture?VP::WordCount:VP::Header)*sizeof(uint32_t);
        if(init)storage=std::make_unique<Buffer>(m_context.GetGraphics(),m_context.GetCommandScheduler(),
            MemoryUsage::Download,0,vk::BufferUsageFlagBits::eStorageBuffer,bytes);
        if(init||capture){
            const auto initialized_words=capture?VP::Header+ds_vertex_count*VP::Stride:VP::Header;
            std::vector<uint32_t> words(initialized_words);
            words[0]=capture?1u:0u;words[1]=ds_vertex_base;words[2]=ds_vertex_count;
            std::memcpy(storage->Mapped().data(),words.data(),initialized_words*sizeof(uint32_t));
            storage->Flush(0,initialized_words*sizeof(uint32_t));
        }
        bindings.vertex[0].shared_memory=vk::DescriptorBufferInfo{storage->Handle(),0,bytes};
    }
    if(root_capture_inputs)Record({{"kind","witness_vertex_capture"},{"draw",root_draw},{"frame",root_frame},
        {"status",ds_vertex_status},{"native_vs_file",ShaderDump::ModuleFile(state.programs.vertex[0].module)},
        {"first_vertex",emit.first_vertex},{"base_vertex",emit.vertex_offset},{"first_instance",emit.first_instance},
        {"vertices",ds_vertex_count},{"sky_pair",ds_vertex_sky_pair}});
	IndirectDrawFixup::Records indirect_records;
	if (indirect != nullptr && !mesh_active) {
		const auto* vertex_program = state.vertex_info[0].stage.program;
		indirect_records = m_indirect_fixup->Record(buffer, *indirect, indirect_sources,
		    vertex_program ? vertex_program->shader_hash : 0);
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
	// Capture the exact material draw with two temporary late-test pipelines.
	// The first retains all rasterized fragments by replacing guest discard
	// with a return; the second keeps the original discard. Both have zero
	// color write masks. The real draw always uses its original pipeline.
	constexpr uint32_t material_queries = 6;
	std::array<vk::Pipeline, 2> material_pipelines {};
	vk::QueryPool material_query_pool {};
	const auto& graphics = m_context.GetGraphics();
	const char* material_query_status = "not_requested";
	if (ds_live_material_capture) {
		const auto& depth = state.depth_info;
		const auto& pixel = state.ps_input_info;
		const bool independent_stencil = !depth.stencil_test_enable ||
		    (depth.stencil_front.compareOp == vk::CompareOp::eAlways &&
		     depth.stencil_back.compareOp == vk::CompareOp::eAlways);
		if (!graphics.occlusion_query_precise_enabled) {
			material_query_status = "precise_occlusion_unsupported";
		} else if (mesh_active || indirect != nullptr || shader_write_stages || image_side_effects ||
		           !depth.image_id || !depth.depth_test_enable || depth.depth_write_enable ||
		           depth.depth_bounds_test_enable || !independent_stencil ||
		           depth.desc.info.samples != 1 || pixel.ps_depth_export_enable ||
		           pixel.ps_sample_mask_export_enable || buffer.GetRegisters().GetShaderRegisters().AlphaToMask()) {
			material_query_status = "draw_has_unsupported_side_effects_or_coverage_state";
		} else {
			material_query_status = "replay_shader_unavailable";
			auto replay_programs = state.programs;
			for (size_t i = 0; i < material_pipelines.size(); ++i) {
				const auto& replay = state.programs.pixel.material_coverage[i];
				if (!replay.module) break;
				replay_programs.pixel = {.id = replay.id, .module = replay.module};
				const auto* replay_pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
				    std::span{state.color_info, state.color_count}, depth, vertex_stages, buffer,
				    &pixel, topology, primitive_restart_enable, replay_programs, true);
				if (!replay_pipeline) break;
				material_pipelines[i] = replay_pipeline->pipeline;
			}
			if (material_pipelines[0] && material_pipelines[1]) {
				vk::QueryPoolCreateInfo info {};
				info.queryType = vk::QueryType::eOcclusion;
				info.queryCount = material_queries;
				if (graphics.device.createQueryPool(&info, nullptr, &material_query_pool) ==
				    vk::Result::eSuccess) {
					m_context.GetCommandScheduler().EndRendering(PassEnd::Buffer);
					buffer.Handle().resetQueryPool(material_query_pool, 0, material_queries);
					material_query_status = "recorded";
				} else {
					material_query_status = "query_pool_unavailable";
				}
			}
		}
	}
	// One post-transition frame: measure the REAL depth and material draws,
	// rather than replaying the first draw of each of five chosen PS hashes.
	// Each query brackets the original draw; one pool, no extra draw, no
	// forced depth comparison, no per-draw GPU/CPU synchronization.
	struct DsGeometryFrame {
		vk::QueryPool pool {};
		int32_t frame = -1;
		uint64_t last_tick = 0;
		int32_t phase = -1;
		std::array<bool,5> completed {};
		bool capacity_reached = false;
		std::array<bool,DemonsDominantGeometry::DepthProbeShaders.size()> replay_seen {};
		nlohmann::json rows = nlohmann::json::array();
	};
	static DsGeometryFrame regular_geometry, active_geometry;
    const auto* geometry_ps=state.ps_active?state.ps_input_info.stage.program:nullptr;
    const auto& geometry_resources=state.ps_input_info.stage.resources;
    const auto active_slot=geometry_ps?DemonsDominantGeometry::DepthProbeSlot(geometry_ps->shader_hash):
        DemonsDominantGeometry::DepthProbeShaders.size();
    const bool active_candidate=active_slot<DemonsDominantGeometry::DepthProbeShaders.size() &&
        geometry_resources && !geometry_resources->images.empty() &&
        geometry_resources->images.front().dwords[0] &&
        state.depth_info.image_id && state.depth_info.desc.info.data.address==DemonsDominantGeometry::DepthAddress;
    auto& geometry=(!root_trace&&active_candidate && !active_geometry.completed[4])?active_geometry:regular_geometry;
	const uint32_t geometry_capacity = root_trace?4096u:512u;
	uint32_t geometry_query = UINT32_MAX;
	uint32_t geometry_replay_first = UINT32_MAX;
	std::array<vk::Pipeline,3> geometry_replay {};
	if (Config::GetShaderDumpFolder().filename() == "ds-lighting-dumps" &&
	    graphics.occlusion_query_precise_enabled) {
		const auto drain_geometry=[&](DsGeometryFrame& geometry){
        const auto window=root_trace?0:geometry.phase==4?8:1;
        if(root_trace&&geometry.pool&&ds_gbuffer_frame>geometry.frame&&geometry.last_tick&&
           !m_context.GetCommandScheduler().GetMasterSemaphore().IsFree(geometry.last_tick)){
            m_context.GetCommandScheduler().EndRendering(PassEnd::Buffer);
            m_context.GetCommandScheduler().Finish();
        }
        if (geometry.pool && int64_t{ds_gbuffer_frame} > int64_t{geometry.frame} + window && !geometry.rows.empty() &&
		    geometry.last_tick != 0 && m_context.GetCommandScheduler().GetMasterSemaphore().IsFree(geometry.last_tick)) {
			std::vector<uint64_t> result(geometry.rows.size() * 2);
			const auto status = graphics.device.getQueryPoolResults(
			    geometry.pool, 0, static_cast<uint32_t>(geometry.rows.size()),
			    result.size() * sizeof(uint64_t), result.data(), 2 * sizeof(uint64_t),
			    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
			bool ready = status == vk::Result::eSuccess;
			for (size_t i = 0; ready && i < geometry.rows.size(); ++i)
				ready = result[2*i+1] != 0;
			if (ready) {
				for (size_t i = 0; i < geometry.rows.size(); ++i)
					geometry.rows[i]["passed_samples"] = result[2*i];
                if(root_trace){
                    for(auto& row:geometry.rows)Record({{"kind","geometry_query"},{"frame",row.value("frame",geometry.frame)},
                        {"capture_frame",geometry.frame},{"result",std::move(row)}});
                    Record({{"kind","geometry_query_end"},{"frame",geometry.frame},
                        {"rows",result.size()/2},{"capacity_reached",geometry.capacity_reached}});
                }else{
                    const auto folder = Config::GetShaderDumpFolder() / "queue-state";
                    std::error_code ec;std::filesystem::create_directories(folder, ec);
                    const auto name = fmt::format("ds_geometry_coverage_f{}{}.json", geometry.frame,geometry.phase==4?"_active":"");
                    std::ofstream file(folder / name, std::ios::trunc);
                    if (file) file << nlohmann::json{{"frame", geometry.frame}, {"phase", geometry.phase},
                        {"capacity_reached", geometry.capacity_reached}, {"draws", geometry.rows}};
                    file.close();
                }
				graphics.device.destroyQueryPool(geometry.pool, nullptr);
				geometry.pool = nullptr;
				geometry.completed[static_cast<size_t>(geometry.phase)] = true;
				geometry.rows.clear();
				geometry.last_tick = 0;
			}
		}
		};
        drain_geometry(regular_geometry);
        drain_geometry(active_geometry);
		const bool relevant_depth = state.depth_info.image_id &&
		    state.depth_info.desc.info.data.address == 0x282b60000ull;
		const auto colors = std::span{state.color_info, state.color_count};
		const auto gbuffer = std::ranges::find_if(colors, [](const auto& color) {
			return color.image_id && color.desc.info.data.address == DemonsQueueProbe::GBufferAddress &&
			       color.desc.view_info.base_layer == 0 && color.desc.view_info.base_level == 0;
		});
		const bool active_window=&geometry==&active_geometry;
        const bool relevant = active_window ? active_candidate : (relevant_depth || gbuffer != colors.end());
		const auto& geometry_gate = DemonsAdaptiveCapture::Current();
		const int geometry_phase = root_trace?root_phase:active_window ? 4 : geometry_gate.PostTransitionDarkDue(ds_gbuffer_frame) ? 3
		    : geometry_gate.DarkOnsetDue(ds_gbuffer_frame) ? 1
		    : geometry_gate.BrightBaselineDue(ds_gbuffer_frame) ? 0
		    : geometry_gate.CaptureDue(ds_gbuffer_frame) ? 2 : -1;
		if (!geometry.pool && relevant && geometry_phase >= 0 &&
		    (root_trace||!geometry.completed[static_cast<size_t>(geometry_phase)])) {
			vk::QueryPoolCreateInfo info {};
			info.queryType = vk::QueryType::eOcclusion;
			info.queryCount = geometry_capacity;
			if (graphics.device.createQueryPool(&info, nullptr, &geometry.pool) == vk::Result::eSuccess) {
				geometry.frame = ds_gbuffer_frame;
				geometry.phase = geometry_phase;
				geometry.capacity_reached = false;
				geometry.replay_seen = {};
				geometry.rows.clear();
				geometry.last_tick = 0;
				m_context.GetCommandScheduler().EndRendering(PassEnd::Buffer);
				buffer.Handle().resetQueryPool(geometry.pool, 0, geometry_capacity);
			} else {
				geometry.pool = nullptr;
				geometry.completed[static_cast<size_t>(geometry_phase)] = true;
				Log::WriteToConsoleAndLog(fmt::format("[DS_GEOMETRY_COVERAGE] phase={} frame={} query_pool_failed=true\n", geometry_phase, ds_gbuffer_frame));
			}
		}
		// Guest flips can split depth prepass and G-buffer across adjacent host
		// frame numbers (213553: depth=429, material=430). Keep both.
		const auto window=root_trace?0:active_window?8:1;
        if (geometry.pool && relevant && int64_t{ds_gbuffer_frame} >= geometry.frame &&
		    int64_t{ds_gbuffer_frame} <= int64_t{geometry.frame} + window &&
		    geometry.rows.size() >= geometry_capacity) geometry.capacity_reached = true;
		if (geometry.pool && int64_t{ds_gbuffer_frame} >= geometry.frame &&
		    int64_t{ds_gbuffer_frame} <= int64_t{geometry.frame} + window && relevant &&
		    geometry.rows.size() < geometry_capacity) {
			geometry_query = static_cast<uint32_t>(geometry.rows.size());
			const auto& hw = buffer.GetRegisters();
			const auto shader_mask = hw.GetShaderRegisters().m_cbShaderMask;
			const auto target_mask = hw.GetRenderTargetMask();
			const auto* vs = state.vertex_info[0].stage.program;
			const auto* ps = state.ps_active ? state.ps_input_info.stage.program : nullptr;
			geometry.rows.push_back({
			    {"frame", ds_gbuffer_frame}, {"submit", submit_id}, {"vs", fmt::format("{:016x}", vs ? vs->shader_hash : 0ull)},
			    {"ps", fmt::format("{:016x}", ps ? ps->shader_hash : 0ull)},
			    {"index_guest", index_source.address}, {"index_bytes", index_source.size},
			    {"index_count_or_records", draw.index_count}, {"instances", draw.instance_count},
			    {"vertex_offset", emit.vertex_offset}, {"first_vertex", emit.first_vertex},
			    {"first_instance", emit.first_instance}, {"indirect", indirect != nullptr},
			    {"indirect_guest", indirect ? indirect->args_addr : 0ull},
			    {"mesh", mesh_active}, {"depth_target", relevant_depth},
			    {"depth_test", state.depth_info.depth_test_enable}, {"depth_write", state.depth_info.depth_write_enable},
			    {"depth_compare", static_cast<uint32_t>(state.depth_info.depth_compare_op)},
			    {"depth_load_clear", state.depth_info.depth_load_clear_enable},
			    {"guest_depth_clear", state.depth_info.depth_clear_enable},
			    {"htile_pending", state.depth_info.trace_meta_pending},
			    {"htile_fill_known", state.depth_info.trace_meta_known},
			    {"htile_fill_word", state.depth_info.trace_meta_fill},
			    {"htile_depth_clear", state.depth_info.trace_meta_depth_clear},
			    {"depth_clear_value", state.depth_info.depth_clear_value},
			    {"stencil_test", state.depth_info.stencil_test_enable},
			    {"gbuffer_layer0", gbuffer != colors.end()},
			    {"gbuffer_write_mask", gbuffer != colors.end() ?
			        GuestColorWriteMask(target_mask, shader_mask, gbuffer->target_slot) : 0u},
			    {"gbuffer_load_clear", gbuffer != colors.end() ?
			        rendering.color_attachments[gbuffer->target_slot].is_clear : false},
			    {"target_mask", target_mask}, {"shader_mask", shader_mask}});
			geometry.rows[geometry_query]["kind"] = "original";
            geometry.rows[geometry_query]["journal_draw"]=root_draw;
            geometry.rows[geometry_query]["command"]=command_seq;
			const auto& capture_face = hw.GetModeControl();
			geometry.rows[geometry_query]["guest_cull_front"] = bool(capture_face.cull_front);
			geometry.rows[geometry_query]["guest_cull_back"] = bool(capture_face.cull_back);
			geometry.rows[geometry_query]["guest_face"] = uint32_t(capture_face.face);
			// Isolate geometry/rasterization from fragment discard and
			// depth/stencil for the measured zero-coverage scene-depth PS.
			const auto& depth = state.depth_info;
			const auto& pixel = state.ps_input_info;
            if(root_trace)geometry.rows[geometry_query]["replay_status"]="unsupported_or_unavailable";
			if (ps && ((root_trace&&(relevant_depth||gbuffer!=colors.end())&&depth.image_id)||
                       (!root_trace&&DemonsDominantGeometry::ProbeDepthPs(ps->shader_hash)&&relevant_depth&&depth.depth_write_enable)) &&
                draw.IsIndexed() &&
			    indirect == nullptr && !mesh_active && !mesh_emulated &&
			    !shader_write_stages && !image_side_effects &&
			    !depth.depth_bounds_test_enable && depth.desc.info.samples == 1 &&
			    !pixel.ps_depth_export_enable && !pixel.ps_sample_mask_export_enable &&
			    !hw.GetShaderRegisters().AlphaToMask() &&
			    geometry.rows.size()+9 <= geometry_capacity) {
				const auto k = DemonsDominantGeometry::DepthProbeSlot(ps->shader_hash);
				if (root_trace || !geometry.replay_seen[k]) {
                    auto& original=geometry.rows[geometry_query];
                    original["probe_slot"]=k;
                    original["replay_status"]="unavailable";
                    if(k<geometry.replay_seen.size())geometry.replay_seen[k]=true;
                    original["tick"]=m_context.GetCommandScheduler().CurrentTick();
                    auto encode=[](const auto& stage){
                        nlohmann::json data=nlohmann::json::object();
                        if(!stage.resources)return data;
                        const auto& snap=*stage.resources;
                        data["srt"]=snap.flattened_srt;data["user_data"]=snap.user_data;
                        data["buffers"]=nlohmann::json::array();
                        for(const auto& v:snap.buffers)data["buffers"].push_back(v.dwords);
                        data["images"]=nlohmann::json::array();
                        for(const auto& v:snap.images)data["images"].push_back(v.dwords);
                        data["samplers"]=nlohmann::json::array();
                        for(const auto& v:snap.samplers)data["samplers"].push_back(v.dwords);
                        return data;
                    };
                    original["vertex_inputs"]=encode(state.vertex_info[0].stage);
                    original["pixel_inputs"]=encode(state.ps_input_info.stage);
                    original["native_ps_file"]=ShaderDump::ModuleFile(state.programs.pixel.module);
                    original["depth_owner"]=m_context.GetTextureCache().GetImage(depth.image_id).info.data.address;
                    original["depth_version_before"]=m_context.GetTextureCache().GetImage(depth.image_id).ContentVersion();
                    original["stencil_compare_front"]=uint32_t(depth.stencil_front.compareOp);
                    original["stencil_compare_back"]=uint32_t(depth.stencil_back.compareOp);
                    original["stencil_reference_front"]=depth.stencil_front.reference;
                    original["stencil_reference_back"]=depth.stencil_back.reference;
                    if(!depth.depth_load_clear_enable){
                        const auto depth_grid=SnapshotDsMaterialDepthGrid(m_context,depth);
                        if(depth_grid)original["stored_depth_grid_before"]=*depth_grid;
                    }
                    original["native_vs_file"]=ShaderDump::ModuleFile(state.programs.vertex[0].module);
                    if(!root_trace&&bindings.pixel && state.ps_input_info.stage.resources &&
                       !state.ps_input_info.stage.resources->images.empty()){
                        const auto& raw=state.ps_input_info.stage.resources->images.front().dwords;
                        original["discard_input"]=SnapshotDsDiscardImage(m_context,*bindings.pixel,raw);
                    }else{
                        original["discard_input"]={{"status","descriptor_or_binding_unavailable"}};
                    }
					auto replay_programs = state.programs;
					for (size_t i=0; i<geometry_replay.size(); ++i) {
						const auto& replay = state.programs.pixel.material_coverage[i == 2 ? 0 : i];
						if (!replay.module) break;
						replay_programs.pixel = {.id = replay.id,.module = replay.module};
						const auto* rp = m_context.GetPipelineCache().GetGraphicsPipeline(
						    colors,depth,vertex_stages,buffer,&pixel,topology,
						    primitive_restart_enable,replay_programs,true,i == 2);
						if (!rp) break;
						geometry_replay[i] = rp->pipeline;
					}
					if (geometry_replay[0] && geometry_replay[1] && geometry_replay[2]) {
						if(k<geometry.replay_seen.size())geometry.replay_seen[k] = true;
                        geometry.rows[geometry_query]["replay_status"]="measured";
						geometry_replay_first = static_cast<uint32_t>(geometry.rows.size());
						for (const auto* kind: {"raster_no_discard","survive_discard","raster_no_discard_no_cull",
                                "survive_depth_only","survive_stencil_only","survive_both_readonly",
                                "survive_depth_less","survive_depth_equal","survive_depth_greater"}) {
							auto row = geometry.rows[geometry_query];
							row["kind"] = kind;
							row["original_query"] = geometry_query;
                            row.erase("discard_input");
                            if(root_trace){row.erase("vertex_inputs");row.erase("pixel_inputs");row.erase("stored_depth_grid_before");}
							const auto q=geometry.rows.size()-geometry_replay_first;
                            row["depth_test"] = (q==3 || q==5 || q>=6) && depth.depth_test_enable;
                            if(q>=6)row["depth_compare"]=uint32_t(q==6?vk::CompareOp::eLess:q==7?vk::CompareOp::eEqual:vk::CompareOp::eGreater);
                            row["depth_write"] = false;
                            row["stencil_test"] = (q==4 || q==5) && depth.stencil_test_enable;
                            row["stencil_write_mask"]=0;
                            row["cull_disabled"] = q==2;
							geometry.rows.push_back(std::move(row));
						}
					}
				}
			}
		}
	}
	if(root_draw){
        const auto& regs=buffer.GetRegisters();
        nlohmann::json event={{"kind","draw_execute"},{"draw",root_draw},{"frame",root_frame},
            {"submit",submit_id},{"tick",m_context.GetCommandScheduler().CurrentTick()},
            {"depth_test",state.depth_info.depth_test_enable},{"depth_write",state.depth_info.depth_write_enable},
            {"depth_compare",uint32_t(state.depth_info.depth_compare_op)},
            {"depth_clear",state.depth_info.depth_load_clear_enable},
            {"htile_pending",state.depth_info.trace_meta_pending}};
        if(root_color.image_id){
            event["image"]=Identity(m_context.GetTextureCache().GetImage(root_color.image_id));
            event["load_clear"]=rendering.color_attachments[root_color.target_slot].is_clear;
            event["write_mask"]=GuestColorWriteMask(regs.GetRenderTargetMask(),
                regs.GetShaderRegisters().m_cbShaderMask,root_color.target_slot);
        }
        if(root_capture_inputs){
            const bool begins_cycle=root_cycle.boundary.Observe(root_draw,root_depth,state.depth_info.depth_load_clear_enable);
            const bool retained=root_cycle.retained.Retain(root_draw,std::move(root_pending_input),
                begins_cycle,root_cycle.boundary.first!=0,{},root_pending_binary.size());
            if(begins_cycle){
                root_cycle.binary.clear();root_cycle.positions.clear();
                root_cycle.position_bytes=0;root_cycle.position_dropped=0;
                root_cycle.frame=root_frame;root_cycle.first_submit=submit_id;
                Record({{"kind","witness_depth_cycle_begin"},{"frame",root_frame},{"draw",root_draw},
                    {"submit",submit_id},{"first_input_retained",retained},
                    {"scope","executed clear, input admitted after resetting previous cycle budget"}});
            }
            if(retained&&!root_pending_binary.empty())root_cycle.binary.emplace(root_draw,std::move(root_pending_binary));
            if(!root_cycle.boundary.first){
                // Prune payloads evicted from the pre-boundary rolling prefix.
                std::erase_if(root_cycle.binary,[&](const auto& entry){
                    return std::ranges::none_of(root_cycle.retained.inputs,[&](const auto& input){return input.first==entry.first;});
                });
            }
            Record({{"kind","witness_input_retention"},{"frame",root_frame},{"draw",root_draw},
                {"submit",submit_id},{"retained",retained},{"cycle_first_draw",root_cycle.boundary.first},
                {"bytes",root_cycle.retained.bytes},{"dropped",root_cycle.retained.dropped}});
        }
        Record(std::move(event));
    }
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
	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(),
	                           state.ps_active && state.ps_input_info.stage.program
	                               ? state.ps_input_info.stage.program->shader_hash : 0,
	                           state.depth_info, rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	dynamic_state.Commit(m_context.GetGraphics(), vk_buffer);
	const auto material_depth_compare = dynamic_state.depth_compare_op;
    if(root_capture_inputs){
        const auto& vp=dynamic_state.viewports[0];
        const auto& sc=dynamic_state.scissors[0];
        Record({{"kind","witness_dynamic_state"},{"frame",root_frame},{"draw",root_draw},
            {"submit",submit_id},{"viewport",{vp.x,vp.y,vp.width,vp.height,vp.minDepth,vp.maxDepth}},
            {"scissor",{sc.offset.x,sc.offset.y,sc.extent.width,sc.extent.height}},
            {"viewport_count",dynamic_state.num_viewports},
            {"dx_clip_space",bool(buffer.GetRegisters().GetClipControl().dx_clip_space)},
            {"depth_compare",uint32_t(material_depth_compare)},
            {"depth_bias",dynamic_state.depth_bias_enabled},
            {"depth_bias_values",{dynamic_state.depth_bias_constant,dynamic_state.depth_bias_slope,dynamic_state.depth_bias_clamp}},
            {"guest_cull_front",bool(buffer.GetRegisters().GetModeControl().cull_front)},
            {"guest_cull_back",bool(buffer.GetRegisters().GetModeControl().cull_back)},
            {"guest_face",uint32_t(buffer.GetRegisters().GetModeControl().face)}});
    }

	// DS shadow casters supply a separate PA_SU_POLY_OFFSET_* for front and
	// back faces. When this title-scoped path is enabled the pipeline exposes
	// dynamic culling, which must be explicitly set on *every* draw (including
	// ordinary draws) to avoid inheriting the preceding split face.
	static const bool ds_per_face_bias = [] {
		if(DemonsPcRenderParity::Enabled())return false;
		const char* setting = std::getenv("KYTY_DS_PER_FACE_BIAS");
		return setting != nullptr && setting[0] == '1' && setting[1] == '\0';
	}();
	const auto& face_mode = buffer.GetRegisters().GetModeControl();
	const auto& face_offset = buffer.GetRegisters().GetPolyOffset();
	const bool rect_face = Prospero::IsRectList(ucfg.GetPrimType());
	const bool front_bias = face_mode.poly_offset_front_enable && !face_mode.cull_front;
	const bool back_bias  = face_mode.poly_offset_back_enable && !face_mode.cull_back;
	vk::CullModeFlags base_cull {};
	if (!rect_face && face_mode.cull_front) base_cull |= vk::CullModeFlagBits::eFront;
	if (!rect_face && face_mode.cull_back) base_cull |= vk::CullModeFlagBits::eBack;
	if (ds_per_face_bias) {
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
		if (geometry_replay_first != UINT32_MAX) {
			vk_buffer.setDepthWriteEnable(false);
			vk_buffer.setDepthTestEnable(false);
			vk_buffer.setStencilTestEnable(false);
			for (uint32_t i=0; i<geometry_replay.size(); ++i) {
				vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,geometry_replay[i]);
				if (ds_per_face_bias) vk_buffer.setCullMode(i == 2 ? vk::CullModeFlags {} : base_cull);
				vk_buffer.beginQuery(geometry.pool,geometry_replay_first+i,vk::QueryControlFlagBits::ePrecise);
				issue_draw();
				vk_buffer.endQuery(geometry.pool,geometry_replay_first+i);
			}
			if (ds_per_face_bias) vk_buffer.setCullMode(base_cull);
            // Same keep-discard module, with independent late tests, never
            // modifying depth, stencil or color. Restore all dynamic state.
            vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,geometry_replay[1]);
            vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,0);
            for(uint32_t q=3;q<9;++q){
                vk_buffer.setDepthTestEnable((q==3 || q==5 || q>=6) && state.depth_info.depth_test_enable);
                if(q>=6)vk_buffer.setDepthCompareOp(q==6?vk::CompareOp::eLess:q==7?vk::CompareOp::eEqual:vk::CompareOp::eGreater);
                vk_buffer.setStencilTestEnable((q==4 || q==5) && state.depth_info.stencil_test_enable);
                vk_buffer.beginQuery(geometry.pool,geometry_replay_first+q,vk::QueryControlFlagBits::ePrecise);
                issue_draw();
                vk_buffer.endQuery(geometry.pool,geometry_replay_first+q);
            }
            vk_buffer.setDepthCompareOp(material_depth_compare);
            vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,dynamic_state.stencil_front_write_mask);
            vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eBack,dynamic_state.stencil_back_write_mask);
			vk_buffer.setDepthWriteEnable(state.depth_info.depth_write_enable);
			vk_buffer.setDepthTestEnable(state.depth_info.depth_test_enable);
			vk_buffer.setStencilTestEnable(state.depth_info.stencil_test_enable);
			vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,pipeline.pipeline);
		}
		if (material_query_pool) {
			const auto measure = [&](uint32_t query, vk::Pipeline replay, vk::CompareOp compare) {
				vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, replay);
				vk_buffer.setDepthCompareOp(compare);
				vk_buffer.beginQuery(material_query_pool, query, vk::QueryControlFlagBits::ePrecise);
				issue_draw();
				vk_buffer.endQuery(material_query_pool, query);
			};
			vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, 0);
			measure(0, material_pipelines[0], vk::CompareOp::eAlways);
			measure(1, material_pipelines[0], state.depth_info.depth_compare_op);
			measure(2, material_pipelines[1], state.depth_info.depth_compare_op);
			measure(3, material_pipelines[0], vk::CompareOp::eGreaterOrEqual);
			measure(4, material_pipelines[0], vk::CompareOp::eLessOrEqual);
			measure(5, material_pipelines[1], material_depth_compare);
			vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
			                              dynamic_state.stencil_front_write_mask);
			vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eBack,
			                              dynamic_state.stencil_back_write_mask);
			vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
		}
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
		if (geometry_query != UINT32_MAX)
			vk_buffer.beginQuery(geometry.pool, geometry_query, vk::QueryControlFlagBits::ePrecise);
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
		if (geometry_query != UINT32_MAX) {
			vk_buffer.endQuery(geometry.pool, geometry_query);
			geometry.last_tick = m_context.GetCommandScheduler().CurrentTick();
		}
	}

	if(root_draw&&(root_color.image_id||root_background_depth||
        (root_depth&&state.depth_info.depth_write_enable))){
        nlohmann::json event={{"kind","draw_after"},{"draw",root_draw},{"frame",root_frame},
            {"submit",submit_id},{"tick",m_context.GetCommandScheduler().CurrentTick()}};
        if(root_color.image_id){
            auto& im=m_context.GetTextureCache().GetImage(root_color.image_id);
            std::optional<std::array<uint32_t,64>> sample;
            if(AllowSample()){
                sample=SnapshotDsGBufferGrid(m_context,root_color);
                event["full"]=root_population(root_color);
                if(root_sky_material&&!root_witness_latched&&root_full_before.is_object()&&
                   event["full"].is_object()){
                    const auto area=uint64_t{event["full"].value("width",0u)}*event["full"].value("height",0u);
                    root_witness_loss=area&&root_full_before.value("rgb_nonzero",uint64_t{0})>=area/2&&
                        event["full"].value("rgb_nonzero",uint64_t{0})<=area/10;
                    if(root_witness_loss)event["witness_first_material_loss"]=true;
                }
            }
            event["image"]=Identity(im);
            event["grid"]=sample?nlohmann::json(*sample):nlohmann::json();
            if(root_before&&sample){
                const auto d=DemonsPrelightPolicy::Compare(*root_before,*sample);
                event["sample_delta"]={{"changed",d.changed},{"lost",d.lost},{"gained",d.gained},
                    {"rgb_before",d.before},{"rgb_after",d.after}};
            }
        }
        if((root_background_depth||(root_depth&&state.depth_info.depth_write_enable))&&AllowSample()){
            const auto depth=SnapshotDsMaterialDepthGrid(m_context,state.depth_info);
            event["depth_grid"]=depth?nlohmann::json(*depth):nlohmann::json();
            event["depth_image"]=Identity(m_context.GetTextureCache().GetImage(state.depth_info.image_id));
        }
        event["tick_after_sample"]=m_context.GetCommandScheduler().CurrentTick();
        Record(std::move(event));
    }
	if (draw_marks) {
		MarkDrawForLiveKnobs(buffer, state, vertex_stages, rendering, mesh_emulated,
		                     mesh_emulated ? uint64_t {emulated_mesh.index_count}
		                                   : uint64_t {draw.index_count} * draw.instance_count);
	}
    if(ds_vertex_phase>=0){
        const auto viewport=dynamic_state.viewports[0];
        const auto scissor=dynamic_state.scissors[0];
        const auto viewport_count=dynamic_state.num_viewports;
        const auto depth_compare=dynamic_state.depth_compare_op;
        const auto depth_write=dynamic_state.depth_write_enabled;
        const auto depth_test=dynamic_state.depth_test_enabled;
        const auto bias=dynamic_state.depth_bias_enabled;
        const auto bias_values=std::array<float,3>{dynamic_state.depth_bias_constant,
            dynamic_state.depth_bias_slope,dynamic_state.depth_bias_clamp};
        const auto stencil_test=dynamic_state.stencil_test_enabled;
        const auto stencil_front=dynamic_state.stencil_front_ops.compare_op;
        const auto stencil_back=dynamic_state.stencil_back_ops.compare_op;
        const auto front_ref=dynamic_state.stencil_front_reference;
        const auto back_ref=dynamic_state.stencil_back_reference;
        auto& scheduler=m_context.GetCommandScheduler();
        scheduler.EndRendering(PassEnd::Buffer);
        vk::MemoryBarrier host{};
        host.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
        host.dstAccessMask=vk::AccessFlagBits::eHostRead;
        scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader,
            vk::PipelineStageFlagBits::eHost,{},1,&host,0,nullptr,0,nullptr);
        scheduler.Finish();
        auto& storage=m_ds_vertex_position_buffers[1];
        const auto words=VP::Header+size_t{ds_vertex_count}*VP::Stride;
        storage->Invalidate(0,words*sizeof(uint32_t));
        std::vector<uint32_t> values(words);
        std::memcpy(values.data(),storage->Mapped().data(),words*sizeof(uint32_t));
        nlohmann::json rows=nlohmann::json::array();
        uint32_t missing=0;
        std::vector<uint32_t> unique=ds_vertex_indices;
        std::sort(unique.begin(),unique.end());
        unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
        const bool projection=VP::HasProjectionOperands(ds_vp->shader_hash,ds_vp->ds_light_math_variant);
        nlohmann::json projection_rows=nlohmann::json::array();
        size_t ordinal=0;
        for(auto raw:unique){
            const auto idx=static_cast<uint32_t>(int64_t{raw}+ds_vertex_offset);
            const auto p=VP::Header+size_t{idx-ds_vertex_base}*VP::Stride;
            if(!values[p]){++missing;continue;}
            nlohmann::json row=nlohmann::json::array();
            row.push_back(idx);
            for(uint32_t j=1;j<VP::Stride;++j)row.push_back(projection&&j>=8?0u:values[p+j]);
            if(projection&&ordinal++%64==0){
                nlohmann::json sample=nlohmann::json::array();sample.push_back(idx);
                for(uint32_t j=8;j<VP::Stride;++j)sample.push_back(values[p+j]);
                projection_rows.push_back(std::move(sample));
            }
            rows.push_back(std::move(row));
        }
        // Retain the exact region of D32 consumed by this draw. The
        // previous 64-point grid missed these ~12x8-pixel meshes entirely.
        // Restrict exported regions to at most 65536 pixels, but keep the
        // actual projected bounds and a reason if a region is unavailable.
        auto depth_after=(!root_trace||ds_vertex_sky_pair)?SnapshotDsDepthFull(m_context,state.depth_info):DsDepthFullSnapshot{};
        nlohmann::json depth_region={{"status","unavailable"}};
        double minx=std::numeric_limits<double>::infinity(),miny=minx;
        double maxx=-minx,maxy=-minx;
        for(const auto& row:rows){
            const float x=std::bit_cast<float>(row[3].get<uint32_t>());
            const float y=std::bit_cast<float>(row[4].get<uint32_t>());
            const float w=std::bit_cast<float>(row[6].get<uint32_t>());
            if(!(std::isfinite(x)&&std::isfinite(y)&&std::isfinite(w)) || w<=0)continue;
            const double px=viewport.x+(double{x}/w+1.0)*viewport.width*0.5;
            const double py=viewport.y+(double{y}/w+1.0)*viewport.height*0.5;
            minx=std::min(minx,px);maxx=std::max(maxx,px);
            miny=std::min(miny,py);maxy=std::max(maxy,py);
        }
        if(std::isfinite(minx)&&std::isfinite(miny)&&viewport_count==1 &&
           !ds_vertex_depth_before.bits.empty() &&
           ds_vertex_depth_before.width==depth_after.width &&
           ds_vertex_depth_before.height==depth_after.height &&
           ds_vertex_depth_before.bits.size()==depth_after.bits.size()){
            const auto width=depth_after.width,height=depth_after.height;
            const auto bound=[](double x,uint32_t limit){return static_cast<uint32_t>(std::clamp(x,0.0,static_cast<double>(limit)));};
            const uint32_t x0=bound(std::floor(minx)-2,width),x1=bound(std::ceil(maxx)+2,width);
            const uint32_t y0=bound(std::floor(miny)-2,height),y1=bound(std::ceil(maxy)+2,height);
            depth_region={{"status","outside_or_too_large"},{"image_extent",{width,height}},
                {"projected_bounds",{minx,miny,maxx,maxy}},{"rect",{x0,y0,x1,y1}}};
            // The dominant surface covers the full attachment. Include
            // a regular 16x16 comparison and exact aggregate changes even
            // when a full-resolution rectangular dump would be too large.
            std::vector<uint32_t> before_grid,after_grid;
            for(uint32_t y=0;y<16;++y)for(uint32_t x=0;x<16;++x){
                const size_t p=size_t{(2*y+1)*height/32}*width+(2*x+1)*width/32;
                before_grid.push_back(ds_vertex_depth_before.bits[p]);
                after_grid.push_back(depth_after.bits[p]);
            }
            uint64_t changed=0,filled_before=0,filled_after=0;
            for(size_t p=0;p<depth_after.bits.size();++p){
                changed+=depth_after.bits[p]!=ds_vertex_depth_before.bits[p];
                filled_before+=ds_vertex_depth_before.bits[p]!=0;
                filled_after+=depth_after.bits[p]!=0;
            }
            depth_region["grid_before"]=std::move(before_grid);
            depth_region["grid_after"]=std::move(after_grid);
            depth_region["changed_pixels"]=changed;
            depth_region["filled_before"]=filled_before;
            depth_region["filled_after"]=filled_after;
            if(x1>x0 && y1>y0 && uint64_t{x1-x0}*(y1-y0)<=65536){
                std::vector<uint32_t> before,after;
                before.reserve(size_t{x1-x0}*(y1-y0));after.reserve(before.capacity());
                for(uint32_t y=y0;y<y1;++y){
                    const size_t off=size_t{y}*width+x0;
                    before.insert(before.end(),ds_vertex_depth_before.bits.begin()+off,
                        ds_vertex_depth_before.bits.begin()+off+(x1-x0));
                    after.insert(after.end(),depth_after.bits.begin()+off,
                        depth_after.bits.begin()+off+(x1-x0));
                }
                depth_region["status"]="captured";
                depth_region["before_bits"]=std::move(before);
                depth_region["after_bits"]=std::move(after);
            }
        }
        const auto& snap=*state.vertex_info[0].stage.resources;
        auto descriptors=nlohmann::json::array();
        for(const auto& desc:snap.buffers)descriptors.push_back(desc.dwords);
        const auto folder=Config::GetShaderDumpFolder()/"queue-state";
        std::error_code ec;std::filesystem::create_directories(folder,ec);
        const auto name=root_trace?
            fmt::format("ds_prelight_witness_vertex_{}.json",root_draw):
            fmt::format("ds_vertex_position_{:016x}_f{}_s{}_p{}_pair{}.json",
                ds_vp->shader_hash,ds_gbuffer_frame,submit_id,ds_vertex_phase,ds_vertex_pair);
        nlohmann::json position_report={{"frame",ds_gbuffer_frame},{"submit",submit_id},
            {"phase",ds_vertex_phase},{"shader",fmt::format("{:016x}",ds_vp->shader_hash)},
            {"fingerprint",fmt::format("{:016x}",ds_vp->ds_light_math_variant)},
            {"original_vs_file",fmt::format("spv/vs_{:016x}_{:016x}.spv",ds_vp->shader_hash,ds_vp->ds_light_math_variant)},
            {"native_vs_file",ShaderDump::ModuleFile(state.programs.vertex[0].module)},
            {"native_ps_file",ShaderDump::ModuleFile(state.programs.pixel.module)},
            {"depth_owner",state.depth_info.image_id?m_context.GetTextureCache().GetImage(state.depth_info.image_id).info.data.address:0ull},
            {"depth_version",state.depth_info.image_id?m_context.GetTextureCache().GetImage(state.depth_info.image_id).ContentVersion():0ull},
            {"tick",m_context.GetCommandScheduler().CurrentTick()},
            {"index_guest",index_source.address},{"base_vertex",emit.vertex_offset},{"first_vertex",emit.first_vertex},
            {"indexed",draw.IsIndexed()},{"instances",draw.instance_count},
            {"first_instance",emit.first_instance},{"indices",ds_vertex_indices},{"missing",missing},
            {"pair_id",ds_vertex_pair},{"journal_draw",root_draw},{"command",command_seq},
            {"material",root_trace?root_target!=root_colors.end():VP::Material(ds_vp->shader_hash)},
            {"target_layer0",root_target!=root_colors.end()},{"depth_target",root_depth},
            {"pixel_shader",state.ps_input_info.stage.program?
                fmt::format("{:016x}",state.ps_input_info.stage.program->shader_hash):std::string{}},
            {"transform_inputs_valid",VP::HasTransformInputs(ds_vp->shader_hash,ds_vp->ds_light_math_variant)},
            {"projection_operands_valid",projection},
            {"projection_operand_ids",{170,733,739,155,689,710,133,716,135,650,677,137,140,158,671}},
            {"projection_rows",std::move(projection_rows)},
            {"viewport",{viewport.x,viewport.y,viewport.width,viewport.height,viewport.minDepth,viewport.maxDepth}},
            {"scissor",{scissor.offset.x,scissor.offset.y,scissor.extent.width,scissor.extent.height}},
            {"dx_clip_space",bool(buffer.GetRegisters().GetClipControl().dx_clip_space)},
            {"depth_test",depth_test},{"depth_write",depth_write},{"depth_compare",uint32_t(depth_compare)},
            {"depth_bias",bias},{"depth_bias_values",bias_values},
            {"stencil_test",stencil_test},{"stencil_compare_front",uint32_t(stencil_front)},
            {"stencil_compare_back",uint32_t(stencil_back)},{"stencil_reference_front",front_ref},
            {"stencil_reference_back",back_ref},{"depth_load_clear",state.depth_info.depth_load_clear_enable},
            {"depth_clear_value",state.depth_info.depth_clear_value},
            {"depth_region",std::move(depth_region)},
            {"vertex_sources",std::move(ds_vertex_sources)},
            {"pixel_resources",std::move(ds_vertex_pixel)},
            {"srt",snap.flattened_srt},{"user_data",snap.user_data},{"descriptors",std::move(descriptors)},
            {"columns",{"vertex","instance","transform","clip_x","clip_y","clip_z","clip_w","clip_distance",
                "input_x","input_y","input_z","m00","m01","m02","m03","m10","m11","m12","m13","m20","m21","m22","m23"}},
            {"rows",std::move(rows)}};
        bool position_saved=false;
        const auto position_payload=position_report.dump();
        if(root_trace){
            if(position_payload.size()<=64u*1024u*1024u-root_cycle.position_bytes){
                root_cycle.positions[root_draw]=position_payload;
                root_cycle.position_bytes+=position_payload.size();position_saved=true;
            }else ++root_cycle.position_dropped;
            if(ds_vertex_sky_pair){
                const auto alias=VP::Material(ds_vp->shader_hash)?"ds_prelight_witness_sky_material.json":"ds_prelight_witness_sky_prepass.json";
                std::ofstream out(folder/alias,std::ios::trunc);
                if(out)out<<position_payload;
            }
        }else{
            std::ofstream out(folder/name,std::ios::trunc);
            if(out)out<<position_payload;
            out.close();position_saved=out.good();
        }
        if(root_trace&&ds_vertex_sky_pair&&!VP::Material(ds_vp->shader_hash)){
            root_witness_pre_serial=ds_vertex_pair;
            root_witness_pre_before=std::move(ds_vertex_depth_before);
            root_witness_pre_after=std::move(depth_after);
        }else if(root_trace&&ds_vertex_sky_pair&&root_witness_loss){
            const auto write_depth=[&](const char* filename,const DsDepthFullSnapshot& depth){
                if(depth.bits.empty())return false;
                std::ofstream out(folder/filename,std::ios::binary|std::ios::trunc);
                if(!out)return false;
                out.write(reinterpret_cast<const char*>(depth.bits.data()),depth.bits.size()*sizeof(uint32_t));
                out.close();return out.good();
            };
            const bool same_pair=root_witness_pre_serial==ds_vertex_pair;
            const bool before_ok=same_pair&&write_depth("ds_prelight_witness_depth_pre_before.bin",root_witness_pre_before);
            const bool after_ok=same_pair&&write_depth("ds_prelight_witness_depth_pre_after.bin",root_witness_pre_after);
            const bool equal=same_pair&&!root_witness_pre_after.bits.empty()&&
                root_witness_pre_after.width==ds_vertex_depth_before.width&&
                root_witness_pre_after.height==ds_vertex_depth_before.height&&
                root_witness_pre_after.bits==ds_vertex_depth_before.bits;
            const bool material_ok=equal||write_depth("ds_prelight_witness_depth_material_before.bin",ds_vertex_depth_before);
            std::ofstream meta(folder/"ds_prelight_witness_depth.json",std::ios::trunc);
            if(meta)meta<<nlohmann::json{{"frame",ds_gbuffer_frame},{"draw",root_draw},{"pair",ds_vertex_pair},
                {"same_pair_serial",same_pair},{"pre_before_saved",before_ok},{"pre_after_saved",after_ok},
                {"material_before_saved",material_ok},{"material_before_equals_pre_after",equal},
                {"material_width",ds_vertex_depth_before.width},{"material_height",ds_vertex_depth_before.height},
                {"pre_width",root_witness_pre_before.width},{"pre_height",root_witness_pre_before.height},
                {"material_indices",ds_vertex_indices},{"material_position_saved",position_saved}};
            root_witness_pre_before={};root_witness_pre_after={};
        }
        Log::WriteToConsoleAndLog(fmt::format("[DS_VERTEX_POSITION] frame={} shader={:016x} vertices={} missing={} file={} saved={}\n",
            ds_gbuffer_frame,ds_vp->shader_hash,unique.size(),missing,name,position_saved));
    }
    if(root_witness_loss){
        root_witness_latched=true;
        const auto folder=DemonsPrelightTrace::Folder();
        std::error_code ec;std::filesystem::create_directories(folder,ec);
        nlohmann::json saved=nlohmann::json::array();
        std::map<std::string,uint64_t> required;
        const auto require=[&](const std::string& path){
            if(path.empty())return;
            std::error_code error;
            const auto size=std::filesystem::file_size(Config::GetShaderDumpFolder()/path,error);
            required[path]=error?0:size;
        };
        nlohmann::json position_files=nlohmann::json::array();
        for(const auto& [id,bytes]:root_cycle.binary){
            const auto name=fmt::format("ds_prelight_witness_data_{}.bin",id);
            std::ofstream out(folder/name,std::ios::binary|std::ios::trunc);
            if(out)out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            out.close();require("queue-state/"+name);
        }
        for(const auto& [id,line]:root_cycle.positions){
            const auto name=fmt::format("ds_prelight_witness_vertex_{}.json",id);
            std::ofstream out(folder/name,std::ios::trunc);
            if(out)out<<line;
            out.close();require("queue-state/"+name);
            position_files.push_back({{"draw",id},{"file",name},{"saved",out.good()}});
        }
        for(const auto& [id,line]:root_cycle.retained.inputs){
            const auto name=fmt::format("ds_prelight_witness_foreground_{}.json",id);
            std::ofstream out(folder/name,std::ios::trunc);
            if(out)out<<line;
            out.close();
            saved.push_back({{"draw",id},{"file",name},{"saved",out.good()}});
            require("queue-state/"+name);
            const auto input=nlohmann::json::parse(line,nullptr,false);
            if(input.is_object()){
                for(const char* key:{"vs_file","ps_file","vs_original_file"})require(input.value(key,std::string{}));
                for(const char* stage:{"vs","ps"}){
                    const auto hash=input.value(std::string(stage)+"_hash",std::string{});
                    if(!hash.empty()&&hash!="0000000000000000")
                        require(fmt::format("guest-shaders/{}_{}_probe.bin",stage,hash));
                }
            }
        }
        for(const char* name:{"ds_prelight_witness_sky_prepass.json","ds_prelight_witness_sky_material.json",
            "ds_prelight_witness_depth.json","ds_prelight_witness_depth_pre_before.bin",
            "ds_prelight_witness_depth_pre_after.bin"})require(std::string("queue-state/")+name);
        const auto optional_depth="ds_prelight_witness_depth_material_before.bin";
        if(std::filesystem::exists(folder/optional_depth,ec))require(std::string("queue-state/")+optional_depth);
        nlohmann::json required_files=nlohmann::json::array();
        for(const auto& [name,bytes]:required)required_files.push_back({{"file",name},{"bytes",bytes},{"present",bytes!=0}});
        std::ofstream meta(folder/"ds_prelight_witness_foreground_manifest.json",std::ios::trunc);
        if(meta)meta<<nlohmann::json{{"active",root_cycle.boundary.first!=0},{"first_frame",root_cycle.frame},
            {"first_draw",root_cycle.boundary.first},{"first_submit",root_cycle.first_submit},
            {"loss_frame",root_frame},{"loss_draw",root_draw},{"bytes",root_cycle.retained.bytes},
            {"dropped",root_cycle.retained.dropped},{"inputs",saved},
            {"position_bytes",root_cycle.position_bytes},{"position_dropped",root_cycle.position_dropped},
            {"positions",position_files},{"required_files",required_files},{"schema",2},
            {"scope","independent guest VS positions versus native positions on the same consumed scalar and vector bytes"},
            {"decision","POSITION_MATCH or POSITION_DIVERGENCE per supported draw; unsupported operands reported per draw, not as a global build prohibition"}};
        root_cycle={};
        Record({{"kind","witness_latched"},{"frame",root_frame},{"draw",root_draw},
            {"position_captured",ds_vertex_phase>=0},{"pair",ds_vertex_pair},
            {"scope","same draw caused population loss; expected guest output not yet certified"}});
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
		RecordDsMaterialPixels(*ds_material_trace, ds_albedo_before, after,
		                       rendering.color_attachments[ds_gbuffer->target_slot].is_clear);
	}
    if(chain_phase>=0){
        const auto after=SnapshotDsAlbedo(m_context,chain_target);
        const auto diff=CompareMaterialFullFrame(chain_before.words,after.words,chain_before.width,chain_before.height);
        const auto& img=m_context.GetTextureCache().GetImage(chain_target.image_id);
        const auto* vs=state.vertex_info[0].stage.program;
        const auto* ps=state.ps_active?state.ps_input_info.stage.program:nullptr;
        uint64_t lost=0,gained=0,changed_nonzero=0;
        nlohmann::json samples=nlohmann::json::array();
        if(diff.valid && chain_before.words.size()==after.words.size()){
            for(size_t i=0;i<after.words.size();++i){
                const auto a=chain_before.words[i],b=after.words[i];
                const bool ca=(a&0x00ffffffu)!=0,cb=(b&0x00ffffffu)!=0;
                lost+=ca&&!cb;gained+=!ca&&cb;changed_nonzero+=ca&&cb&&a!=b;
            }
            for(uint32_t i=0;i<64;++i){
                const auto x=(i%8u*2u+1u)*chain_before.width/16u;
                const auto y=(i/8u*2u+1u)*chain_before.height/16u;
                const auto k=size_t{y}*chain_before.width+x;
                samples.push_back({x,y,chain_before.words[k],after.words[k]});
            }
        }
        nlohmann::json inputs=nlohmann::json::object();
        if(state.ps_active && state.ps_input_info.stage.resources){
            const auto& snap=*state.ps_input_info.stage.resources;
            inputs["srt"]=snap.flattened_srt;
            inputs["images"]=nlohmann::json::array();
            for(const auto& v:snap.images)inputs["images"].push_back(v.dwords);
            inputs["buffers"]=nlohmann::json::array();
            for(const auto& v:snap.buffers)inputs["buffers"].push_back(v.dwords);
        }
        const auto& regs=buffer.GetRegisters();
        auto& rows=chain_rows[chain_phase];
        rows.push_back({
            {"ordinal",rows.size()},{"frame",ds_gbuffer_frame},{"submit",submit_id},
            {"tick",m_context.GetCommandScheduler().CurrentTick()},
            {"vs",fmt::format("{:016x}",vs?vs->shader_hash:0ull)},
            {"ps",fmt::format("{:016x}",ps?ps->shader_hash:0ull)},
            {"index_guest",index_source.address},{"index_bytes",index_source.size},
            {"indices_or_records",draw.index_count},{"instances",draw.instance_count},
            {"indirect",indirect!=nullptr},{"indirect_guest",indirect?indirect->args_addr:0ull},
            {"owner",img.info.data.address},{"version_before",chain_version_before},
            {"version_after",img.ContentVersion()},{"slot",chain_target.target_slot},
            {"load_clear",rendering.color_attachments[chain_target.target_slot].is_clear},
            {"write_mask",GuestColorWriteMask(regs.GetRenderTargetMask(),
                regs.GetShaderRegisters().m_cbShaderMask,chain_target.target_slot)},
            {"depth_compare",uint32_t(state.depth_info.depth_compare_op)},
            {"depth_owner",state.depth_info.desc.info.data.address},
            {"depth_grid_before",chain_depth?nlohmann::json(*chain_depth):nlohmann::json()},
            {"valid",diff.valid},{"width",chain_before.width},{"height",chain_before.height},
            {"colored_before",diff.colored_before},{"colored_after",diff.colored_after},
            {"changed",diff.changed},{"lost",lost},{"gained",gained},{"changed_nonzero",changed_nonzero},
            {"grid_xy_before_after",std::move(samples)},{"pixel_inputs",std::move(inputs)}});
        const auto folder=Config::GetShaderDumpFolder()/"queue-state";
        std::error_code ec;std::filesystem::create_directories(folder,ec);
        const auto name=fmt::format("ds_albedo_chain_p{}.json",chain_phase);
        std::ofstream out(folder/name,std::ios::trunc);
        if(out)out<<nlohmann::json{{"phase",chain_phase},{"start_frame",chain_start[chain_phase]},
            {"capacity_reached",rows.size()>=chain_limit},{"draws",rows}};
    }
	if (ds_depth_prepass_phase >= 0) {
		const auto depth_full_after = ds_depth_full_before.bits.empty()
		    ? DsDepthFullSnapshot {} : SnapshotDsDepthFull(m_context, ds_depth_prepass_target);
		const auto depth_full = CompareDepthFull(ds_depth_full_before.bits, depth_full_after.bits,
		                                       ds_depth_full_before.width, ds_depth_full_before.height);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_DEPTH_FULL_LAYER] frame={} phase={} submit={} valid={} extent={}x{} "
		    "changed={} filled={}->{} zero={}->{} bbox={},{},{},{}\n",
		    ds_gbuffer_frame, ds_depth_prepass_phase, submit_id, depth_full.valid,
		    ds_depth_full_before.width, ds_depth_full_before.height, depth_full.changed,
		    depth_full.filled_before, depth_full.filled_after,
		    depth_full.far_before, depth_full.far_after,
		    depth_full.min_x, depth_full.min_y, depth_full.max_x, depth_full.max_y));
		const auto after = SnapshotDsMaterialDepthGrid(m_context, ds_depth_prepass_target);
		// Reverse-Z: larger values are nearer. Count which direction the
		// actual depth owner changes the attachment, independently of the
		// occlusion query and of the final material's color.
		uint64_t nearer = 0, farther = 0, nonfinite = 0;
		if (depth_full.valid) {
			for (size_t p = 0; p < depth_full_after.bits.size(); ++p) {
				const float prior = std::bit_cast<float>(ds_depth_full_before.bits[p]);
				const float next = std::bit_cast<float>(depth_full_after.bits[p]);
				if (!std::isfinite(prior) || !std::isfinite(next)) { ++nonfinite; continue; }
				nearer += next > prior;
				farther += next < prior;
			}
		}
		if (ds_depth_prepass_before && after) {
			uint32_t before_nonzero = 0, after_nonzero = 0, changed = 0;
			for (size_t i = 0; i < after->size(); ++i) {
				before_nonzero += (*ds_depth_prepass_before)[i] != 0u;
				after_nonzero += (*after)[i] != 0u;
				changed += (*ds_depth_prepass_before)[i] != (*after)[i];
			}
			const uint64_t shader = state.ps_active && state.ps_input_info.stage.program
			    ? state.ps_input_info.stage.program->shader_hash : 0ull;
			const nlohmann::json evidence {
			    {"frame", ds_gbuffer_frame}, {"phase", ds_depth_prepass_phase},
			    {"submit", submit_id}, {"shader", fmt::format("{:016x}", shader)},
			    {"target", ds_depth_prepass_target.desc.info.data.address},
			    {"full_depth", {
			        {"valid", depth_full.valid},
			        {"width", ds_depth_full_before.width},
			        {"height", ds_depth_full_before.height},
			        {"changed", depth_full.changed},
			        {"filled_before", depth_full.filled_before},
			        {"filled_after", depth_full.filled_after},
			        {"zero_before", depth_full.far_before},
			        {"zero_after", depth_full.far_after},
			        {"bbox", {depth_full.min_x, depth_full.min_y, depth_full.max_x, depth_full.max_y}}
			    }},
			    {"before_bits", *ds_depth_prepass_before}, {"after_bits", *after},
			    {"depth_increased_pixels", nearer}, {"depth_decreased_pixels", farther},
			    {"nonfinite_pixels", nonfinite},
			    {"stencil_compare_back", static_cast<uint32_t>(state.depth_info.stencil_back.compareOp)},
			    {"stencil_reference_front", state.depth_info.stencil_front.reference},
			    {"stencil_reference_back", state.depth_info.stencil_back.reference},
			    {"stencil_write_mask_front", state.depth_info.stencil_front.writeMask},
			    {"stencil_write_mask_back", state.depth_info.stencil_back.writeMask},
			    {"nonzero_before", before_nonzero}, {"nonzero_after", after_nonzero},
			    {"changed", changed}, {"depth_test", state.depth_info.depth_test_enable},
			    {"depth_write", state.depth_info.depth_write_enable},
			    {"depth_compare", static_cast<uint32_t>(state.depth_info.depth_compare_op)},
			    {"vertex_shader", fmt::format("{:016x}",
			         state.vertex_info[0].stage.program
			         ? state.vertex_info[0].stage.program->shader_hash : 0ull)},
			    {"pixel_shader", fmt::format("{:016x}", shader)},
			    {"stencil_test", state.depth_info.stencil_test_enable},
			    {"stencil_compare", static_cast<uint32_t>(state.depth_info.stencil_front.compareOp)},
			    {"load_clear", state.depth_info.depth_load_clear_enable},
			    {"guest_depth_clear", state.depth_info.depth_clear_enable},
			    {"clear_depth_value", state.depth_info.depth_clear_value},
			    {"htile_metadata_pending", state.depth_info.trace_meta_pending},
			    {"htile_fill_known", state.depth_info.trace_meta_known},
			    {"htile_fill_word", state.depth_info.trace_meta_fill},
			    {"htile_caused_depth_clear", state.depth_info.trace_meta_depth_clear},
			    {"htile_caused_stencil_clear", state.depth_info.trace_meta_stencil_clear},
			    {"htile_uniform_depth", state.depth_info.trace_meta_uniform},
			    {"clear_aspects", static_cast<uint32_t>(state.depth_info.load_clear_aspects)},
			    {"index_count", draw.index_count}, {"instances", draw.instance_count}
			};
			const auto folder = Config::GetShaderDumpFolder() / "queue-state";
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);
			static uint32_t depth_capture_serial = 0;
			const auto name = fmt::format("ds_depth_prepass_f{}_s{}_p{}_d{}.json",
			                              ds_gbuffer_frame, submit_id, ds_depth_prepass_phase,
			                              ++depth_capture_serial);
			std::ofstream file(folder / name, std::ios::trunc);
			if (file) file << evidence;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_DEPTH_PREPASS] frame={} phase={} target=0x{:x} "
			    "depth_samples={}/64 -> {}/64 changed={} depth_test={} depth_write={} "
			    "load_clear={} guest_clear={} htile_pending={} fill_known={} fill=0x{:08x} "
			    "meta_depth={} meta_stencil={} uniform={} clear_depth={} "
			    "compare={} vs=0x{:016x} ps=0x{:016x} "
			    "indices={} instances={} file={} saved={}\n",
			    ds_gbuffer_frame, ds_depth_prepass_phase,
			    ds_depth_prepass_target.desc.info.data.address,
			    before_nonzero, after_nonzero, changed,
			    state.depth_info.depth_test_enable, state.depth_info.depth_write_enable,
			    state.depth_info.depth_load_clear_enable,
			    state.depth_info.depth_clear_enable, state.depth_info.trace_meta_pending,
			    state.depth_info.trace_meta_known, state.depth_info.trace_meta_fill,
			    state.depth_info.trace_meta_depth_clear, state.depth_info.trace_meta_stencil_clear,
			    state.depth_info.trace_meta_uniform, state.depth_info.depth_clear_value,
			    static_cast<uint32_t>(state.depth_info.depth_compare_op),
			    state.vertex_info[0].stage.program ? state.vertex_info[0].stage.program->shader_hash : 0ull,
			    shader, draw.index_count, draw.instance_count,
			    name, file.good()));
		} else {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_DEPTH_PREPASS] frame={} phase={} result=depth_snapshot_unavailable\n",
			    ds_gbuffer_frame, ds_depth_prepass_phase));
		}
	}
	std::optional<std::array<uint64_t, material_queries>> material_sample_counts;
	if (ds_live_material_capture) {
		if (material_query_pool) {
			m_context.GetCommandScheduler().Finish();
			std::array<uint64_t, material_queries> counts {};
			const auto result = graphics.device.getQueryPoolResults(
			    material_query_pool, 0, material_queries, sizeof(counts), counts.data(),
			    sizeof(uint64_t), vk::QueryResultFlagBits::e64);
			graphics.device.destroyQueryPool(material_query_pool, nullptr);
			material_query_status = vk::Result::eSuccess == result ? "ok" : "query_read_failed";
			if (result == vk::Result::eSuccess) {
				material_sample_counts = counts;
				if (counts[1] > counts[0] || counts[2] > counts[1] ||
				    counts[3] > counts[0] || counts[4] > counts[0] ||
				    counts[5] > counts[0]) {
					material_query_status = "non_comparable_queries";
				}
			}
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_MATERIAL_FRAGMENT_STAGES] frame={} submit={} phase={} vs=0x{:016x} ps=0x{:016x} "
		    "status={} guest_compare={} effective_compare={} rasterized={} guest_depth_pass={} "
		    "guest_shader_survivors={} ge_depth_pass={} le_depth_pass={} effective_shader_survivors={}\n",
		    ds_gbuffer_frame, submit_id, ds_live_material_phase,
		    label_vs ? label_vs->shader_hash : 0ull, label_ps ? label_ps->shader_hash : 0ull,
		    material_query_status, static_cast<uint32_t>(state.depth_info.depth_compare_op),
		    static_cast<uint32_t>(material_depth_compare),
		    material_sample_counts ? std::to_string((*material_sample_counts)[0]) : "unavailable",
		    material_sample_counts ? std::to_string((*material_sample_counts)[1]) : "unavailable",
		    material_sample_counts ? std::to_string((*material_sample_counts)[2]) : "unavailable",
		    material_sample_counts ? std::to_string((*material_sample_counts)[3]) : "unavailable",
		    material_sample_counts ? std::to_string((*material_sample_counts)[4]) : "unavailable",
		    material_sample_counts ? std::to_string((*material_sample_counts)[5]) : "unavailable"));
	}
	if (ds_live_material_capture) {
		const auto material_depth_full_after = ds_material_depth_full_before.bits.empty()
		    ? DsDepthFullSnapshot {} : SnapshotDsDepthFull(m_context, ds_live_material_depth);
		const auto material_depth_diff = CompareDepthFull(
		    ds_material_depth_full_before.bits, material_depth_full_after.bits,
		    ds_material_depth_full_before.width, ds_material_depth_full_before.height);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_MATERIAL_DEPTH_COVERAGE] frame={} phase={} before={} after={} "
		    "changed={} valid={}\n", ds_gbuffer_frame, ds_live_material_phase,
		    material_depth_diff.filled_before, material_depth_diff.filled_after,
		    material_depth_diff.changed, material_depth_diff.valid));
		const auto full_after = ds_live_material_full_before.words.empty()
		    ? DsAlbedoSnapshot {} : SnapshotDsAlbedo(m_context, ds_live_material_target);
		const auto full_diff = CompareMaterialFullFrame(
		    ds_live_material_full_before.words, full_after.words,
		    ds_live_material_full_before.width, ds_live_material_full_before.height);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_GBUFFER_FULL_LAYER] frame={} phase={} submit={} shader=0x{:016x} "
		    "valid={} extent={}x{} changed={} colored={}->{} bbox={},{},{},{}\n",
		    ds_gbuffer_frame, ds_live_material_phase, submit_id,
		    state.ps_input_info.stage.program->shader_hash, full_diff.valid,
		    ds_live_material_full_before.width, ds_live_material_full_before.height,
		    full_diff.changed, full_diff.colored_before, full_diff.colored_after,
		    full_diff.min_x, full_diff.min_y, full_diff.max_x, full_diff.max_y));
		const auto after = SnapshotDsGBufferGrid(m_context, ds_live_material_target);
		if (after && ds_live_material_before) {
			const auto frame = m_context.GetGpu().GetFrameNum();
			const auto shader = state.ps_input_info.stage.program->shader_hash;
			uint32_t changed = 0, before_colored = 0, after_colored = 0;
			uint32_t before_clear = 0, after_clear = 0;
			const auto depth_after = SnapshotDsMaterialDepthGrid(m_context, ds_live_material_depth);
			uint32_t depth_zero_before = 0, depth_zero_after = 0;
			uint32_t depth_changed = 0;
			if (ds_live_depth_before && depth_after) {
				for (size_t i = 0; i < depth_after->size(); ++i) {
					depth_zero_before += (*ds_live_depth_before)[i] == 0u;
					depth_zero_after += (*depth_after)[i] == 0u;
					depth_changed += (*ds_live_depth_before)[i] != (*depth_after)[i];
				}
			}
			for (size_t i = 0; i < after->size(); ++i) {
				changed += (*ds_live_material_before)[i] != (*after)[i];
				before_colored += ((*ds_live_material_before)[i] & 0x00ffffffu) != 0u;
				after_colored += ((*after)[i] & 0x00ffffffu) != 0u;
				before_clear += (*ds_live_material_before)[i] == 0xff000000u;
				after_clear += (*after)[i] == 0xff000000u;
			}
			const auto& regs = buffer.GetRegisters();
			const auto& dyn = m_context.GetCommandScheduler().GetDynamicState();
			const vk::Rect2D scissor = dyn.num_scissors ? dyn.scissors[0] : vk::Rect2D {};
			const auto& target_image = m_context.GetTextureCache().GetImage(ds_live_material_target.image_id);
			const nlohmann::json payload {
			    {"frame", frame}, {"phase", ds_live_material_phase == 0 ? "bright" :
			         ds_live_material_phase == 1 ? "onset" :
			         ds_live_material_phase == 3 ? "post_transition_dark_candidate" : "dark"},
			    {"submit", submit_id}, {"shader", fmt::format("{:016x}", shader)},
			    {"guest", ds_live_material_target.desc.info.data.address},
			    {"image_owner", target_image.info.data.address},
			    {"image_version", target_image.ContentVersion()},
			    {"before_r32", *ds_live_material_before}, {"after_r32", *after},
			    {"depth_before_bits", ds_live_depth_before ?
			         nlohmann::json(*ds_live_depth_before) : nlohmann::json()},
			    {"depth_after_bits", depth_after ?
			         nlohmann::json(*depth_after) : nlohmann::json()},
			    {"depth_zero_before", depth_zero_before},
			    {"depth_zero_after", depth_zero_after},
			    {"depth_changed", depth_changed},
			    {"material_fragment_stages", {
			        {"status", material_query_status},
			        {"guest_depth_compare", static_cast<uint32_t>(state.depth_info.depth_compare_op)},
			        {"depth_compare", static_cast<uint32_t>(material_depth_compare)},
			        {"vertex_shader", fmt::format("{:016x}", label_vs ? label_vs->shader_hash : 0ull)},
			        {"rasterized", material_sample_counts ? nlohmann::json((*material_sample_counts)[0]) : nlohmann::json()},
			        {"depth_pass", material_sample_counts ? nlohmann::json((*material_sample_counts)[1]) : nlohmann::json()},
			        {"shader_survivors", material_sample_counts ? nlohmann::json((*material_sample_counts)[2]) : nlohmann::json()},
			        {"greater_or_equal_pass", material_sample_counts ? nlohmann::json((*material_sample_counts)[3]) : nlohmann::json()},
			        {"less_or_equal_pass", material_sample_counts ? nlohmann::json((*material_sample_counts)[4]) : nlohmann::json()},
			        {"effective_shader_survivors", material_sample_counts ? nlohmann::json((*material_sample_counts)[5]) : nlohmann::json()},
			    }},
			    {"material_depth_full", {
			        {"valid", material_depth_diff.valid},
			        {"filled_before", material_depth_diff.filled_before},
			        {"filled_after", material_depth_diff.filled_after},
			        {"changed", material_depth_diff.changed}
			    }},
			    {"full_frame", {
			        {"valid", full_diff.valid}, {"width", ds_live_material_full_before.width},
			        {"height", ds_live_material_full_before.height},
			        {"changed", full_diff.changed},
			        {"colored_before", full_diff.colored_before},
			        {"colored_after", full_diff.colored_after},
			        {"bbox", {full_diff.min_x, full_diff.min_y, full_diff.max_x, full_diff.max_y}}
			    }},
			    {"depth_owner", ds_live_material_depth.desc.info.data.address},
			    {"depth_load_clear", state.depth_info.depth_load_clear_enable},
			    {"depth_attachment_clear", state.depth_info.depth_clear_enable},
			    {"depth_write_aspects",
			        static_cast<uint32_t>(state.depth_info.AttachmentWriteAspects())},
			    {"depth_load_clear_aspects",
			        static_cast<uint32_t>(state.depth_info.load_clear_aspects)},
			    {"before_colored", before_colored}, {"after_colored", after_colored},
			    {"before_ff000000", before_clear}, {"after_ff000000", after_clear},
			    {"changed", changed},
			    {"raster", {
			        {"indices", draw.index_count}, {"instances", draw.instance_count},
			        {"indexed", draw.IsIndexed()}, {"mesh", mesh_active},
			        {"render_width", rendering.width}, {"render_height", rendering.height},
			        {"target_mask", regs.GetRenderTargetMask()},
			        {"shader_mask", regs.GetShaderRegisters().m_cbShaderMask},
			        {"depth_test", state.depth_info.depth_test_enable},
			        {"depth_write", state.depth_info.depth_write_enable},
			        {"depth_func", static_cast<uint32_t>(state.depth_info.depth_compare_op)},
			        {"stencil_test", state.depth_info.stencil_test_enable},
			        {"stencil_func", static_cast<uint32_t>(state.depth_info.stencil_front.compareOp)},
			        {"stencil_ref", state.depth_info.stencil_front.reference},
			        {"native_color_write", static_cast<bool>(
			            dyn.color_write_enables[ds_live_material_target.target_slot])},
			        {"load_clear", rendering.color_attachments[ds_live_material_target.target_slot].is_clear},
			        {"scissor", {scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height}}
			    }}
			};
			const auto folder = Config::GetShaderDumpFolder() / "queue-state";
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);
			// Multiple material shaders share a guest submit. Name by shader
			// as well: 723 overwrote the first writer in both dark phases.
			const auto name = fmt::format("ds_gbuffer_material_{}_f{}_s{}_ps{:016x}.json",
			    ds_live_material_phase == 0 ? "bright" :
			    ds_live_material_phase == 1 ? "onset" :
			    ds_live_material_phase == 3 ? "post_transition" : "dark", frame, submit_id, shader);
			std::ofstream file(folder / name, std::ios::trunc);
			if (file) file << payload;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_GBUFFER_MATERIAL] frame={} phase={} shader=0x{:016x} "
			    "changed={}/64 colored={}/64 -> {}/64 clear={}/64 -> {}/64 "
			    "depth={} stencil={} depth_zero={}/64 -> {}/64 depth_changed={} "
			    "load_clear={} file={} saved={}\n",
			    frame, ds_live_material_phase, shader, changed, before_colored, after_colored,
			    before_clear, after_clear,
			    state.depth_info.depth_test_enable, state.depth_info.stencil_test_enable,
			    depth_zero_before, depth_zero_after, depth_changed,
			    rendering.color_attachments[ds_live_material_target.target_slot].is_clear,
			    name, file.good()));
		}
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
		    {"capture_phase", ds_scene_bright ? "bright" :
		                      ds_scene_onset ? "onset" :
		                      ds_scene_post_transition ? "post_transition_dark_candidate" : "paired_dark"},
		    {"bright_baseline", ds_scene_bright},
		    {"dark_onset", ds_scene_onset},
		    {"paired_event", ds_scene_paired},
		    {"post_transition_candidate", ds_scene_post_transition},
		    {"before", DsSceneGridJson(ds_scene_before)}, {"after", DsSceneGridJson(after)},
		    {"changed_cells", changed_cells}, {"inputs", ds_scene_inputs},
		    {"pixel_shader_data", std::vector<uint32_t>(
		        bindings.pixel->shader_data.begin(),
		        bindings.pixel->shader_data.begin() +
		        std::min<size_t>(bindings.pixel->shader_data.size(), 64u))},
		    {"pixel_buffers", nlohmann::json::array()},
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
		const auto& ps_buffers = bindings.pixel->runtime->program->info.buffers;
		for (const auto& resource : ps_buffers) {
			const auto slot = resource.descriptor_index;
			if (slot >= bindings.pixel->buffer_sources.size()) continue;
			const auto& input = bindings.pixel->buffer_sources[slot];
			payload["pixel_buffers"].push_back({
			    {"slot", slot}, {"guest", input.address}, {"bytes", input.size},
			    {"read", resource.read}, {"written", resource.written}});
		}
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
			    "write_aspects=0x{:x} clear_aspects=0x{:x} "
			    "content_version={} seen_102a4={} seen_20926={}\n",
			    n, m_context.GetGpu().GetFrameNum(), address,
			    static_cast<uint32_t>(tracked.backing.format),
			    tracked.backing.extent.width, tracked.backing.extent.height,
			    di.depth_write_enable ? 1 : 0, di.depth_test_enable ? 1 : 0,
			    tracked.IsGpuModified() ? 1 : 0, tracked.IsCpuDirty() ? 1 : 0,
			    static_cast<uint32_t>(di.AttachmentWriteAspects()),
			    static_cast<uint32_t>(di.load_clear_aspects),
			    tracked.ContentVersion(), depth_102a4.load(std::memory_order_relaxed),
			    depth_20926.load(std::memory_order_relaxed)));
		}
	}
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
}

// Record guest-facing registers/arguments before PrepareDrawRenderState can
// reject the command, so "no prepared draw" is visible in the N20 ledger.
static nlohmann::json DsGuestCommand(RenderContext& context,const CommandBuffer& buffer,
    uint64_t submit,const char* kind,uint64_t address,uint32_t count,uint32_t instances,
    uint64_t indirect_args=0,uint64_t count_addr=0,uint32_t stride=0,uint32_t index_limit=0){
    using namespace DemonsPrelightTrace;
    if(!Enabled())return nullptr;
    const auto frame=context.GetGpu().GetFrameNum();
    const auto phase=Phase(frame);
    if(phase<0)return nullptr;
    const auto& hw=buffer.GetRegisters();
    const auto& depth=hw.GetDepthRenderTarget();
    const auto zr=Kernel::AddressFold::Fold(depth.z_read_base_addr);
    const auto zw=Kernel::AddressFold::Fold(depth.z_write_base_addr);
    bool relevant=zr==DemonsDominantGeometry::DepthAddress||zw==DemonsDominantGeometry::DepthAddress;
    nlohmann::json targets=nlohmann::json::array();
    for(uint32_t slot=0;slot<RENDER_COLOR_ATTACHMENTS_MAX;++slot){
        const auto a=Kernel::AddressFold::Fold(hw.GetRenderTarget(slot).base.addr);
        if(a==DemonsQueueProbe::GBufferAddress)relevant=true;
        targets.push_back(a);
    }
    if(!relevant)return nullptr;
    return {{"kind","guest_command"},{"op",kind},{"frame",frame},{"phase",phase},
        {"submit",submit},{"tick",context.GetCommandScheduler().CurrentTick()},
        {"index_guest",address},{"count",count},{"instances",instances},{"indirect_guest",indirect_args},
        {"count_guest",count_addr},{"stride",stride},{"index_limit",index_limit},
        {"targets",std::move(targets)},{"depth_read",zr},{"depth_write",zw},
        {"target_mask",hw.GetRenderTargetMask()},{"shader_mask",hw.GetShaderRegisters().m_cbShaderMask}};
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
    DemonsPrelightTrace::CommandScope ds_cmd(DsGuestCommand(m_context,buffer,submit_id,"indexed",reinterpret_cast<uint64_t>(args.index_addr),args.index_count,args.instance_count));
	if (args.index_count == 0 || args.instance_count == 0) {
        ds_cmd.outcome="zero_count";
		return;
	}

	if (ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset) ||
	    DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
        ds_cmd.outcome="special_render_operation";
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
        ds_cmd.outcome="invalid_vertex_shader";
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
        ds_cmd.outcome="unsupported_topology";
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
        ds_cmd.outcome="prepare_rejected";
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
    DemonsPrelightTrace::CommandScope ds_cmd(DsGuestCommand(m_context,buffer,submit_id,"auto",0,args.vertex_count,args.instance_count));
	if (args.vertex_count == 0 || args.instance_count == 0) {
        ds_cmd.outcome="zero_count";
		return;
	}

	if (ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset) ||
	    DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
        ds_cmd.outcome="special_render_operation";
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
        ds_cmd.outcome="invalid_vertex_shader";
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
        ds_cmd.outcome="unsupported_topology";
		ResetBindings();
		return;
	}
	auto& state = ReusableDrawRenderState();
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
        ds_cmd.outcome="prepare_rejected";
		ResetBindings();
		return;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
        ds_cmd.outcome="rect_missing_exports";
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
    DemonsPrelightTrace::CommandScope ds_cmd(DsGuestCommand(m_context,buffer,submit_id,
        args.indexed?"indirect_indexed":"indirect_auto",args.index_base_addr,args.max_count,1,
        args.args_addr,args.count_addr,args.stride,args.index_buffer_size));
	if (const char* reason = IndirectDrawUnsupported(buffer, args); reason != nullptr) {
        ds_cmd.outcome="cpu_fallback";
        DemonsIndirectWitness::NoteFallback(args.args_addr, reason,
            DemonsPrelightTrace::command_seq, submit_id,
            m_context.GetCommandScheduler().CurrentTick());
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
        ds_cmd.outcome="invalid_vertex_shader";
		return true;
	}
	uc_check(ucfg);
	hw_check(buffer);
	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
        ds_cmd.outcome="unsupported_topology";
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
        ds_cmd.outcome="prepare_rejected";
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
        ds_cmd.outcome="cpu_fallback";
        DemonsIndirectWitness::NoteFallback(args.args_addr, stage_reason,
            DemonsPrelightTrace::command_seq, submit_id,
            m_context.GetCommandScheduler().CurrentTick());
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
