// --gpu-indirect: DRAW_INDIRECT, DRAW_INDEX_INDIRECT and their _MULTI forms executed as Vulkan
// indirect draws, so the command processor never reads arguments that a shader of the open
// command buffer wrote (a synchronous submit-and-wait readback per packet). See
// graphics/host_gpu/renderer/renderIndirect.h for the host side.

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <atomic>

namespace Libs::Graphics {

namespace {

// NGG vertex shaders receive SPI_SHADER_USER_DATA_GS_n in s(8 + n) (ShaderParams user_data_base).
constexpr uint32_t VertexUserDataFirstSgpr = 8;

// The absolute SGPR a packet's user-data destination register names, or false if the vertex
// prologue cannot provide it.
bool ToVertexSgpr(uint32_t location, int8_t& sgpr) {
	if (location == Pm4::SH_NOP) {
		sgpr = -1;
		return true;
	}
	if (location < Pm4::SPI_SHADER_USER_DATA_GS_0 || location > Pm4::SPI_SHADER_USER_DATA_GS_31) {
		return false;
	}
	sgpr = static_cast<int8_t>(VertexUserDataFirstSgpr + (location - Pm4::SPI_SHADER_USER_DATA_GS_0));
	return true;
}

void LogUnsupportedPacket(const char* reason) {
	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
		LOGF("GPU indirect draw: CPU path for %s\n", reason);
	}
}

} // namespace

bool CommandProcessor::TryDrawIndirectOnGpu(uint32_t data_offset, uint32_t max_count,
                                            const volatile uint32_t* count_addr,
                                            uint32_t stride_in_bytes,
                                            IndirectDrawRegisters registers, bool indexed) {
	if (max_count == 0) {
		return false; // Nothing to draw and nothing to read either way.
	}
	IndirectDrawArgs args {};
	args.args_addr  = m_draw_indirect_args_base_addr + data_offset;
	args.stride     = stride_in_bytes;
	args.max_count  = max_count;
	args.count_addr = reinterpret_cast<uint64_t>(count_addr);
	args.indexed    = indexed;
	if ((args.args_addr & 3u) != 0 || (stride_in_bytes & 3u) != 0 || (args.count_addr & 3u) != 0) {
		LogUnsupportedPacket("unaligned records");
		return false;
	}
	if (registers.index_offset != Pm4::SH_NOP) {
		// No draw parameter carries the first index into an SGPR.
		LogUnsupportedPacket("a first-index user SGPR");
		return false;
	}
	args.sgprs.enabled = true;
	if (!ToVertexSgpr(registers.vertex_offset, args.sgprs.vertex_sgpr) ||
	    !ToVertexSgpr(registers.instance_offset, args.sgprs.instance_sgpr)) {
		LogUnsupportedPacket("offsets outside the vertex user data");
		return false;
	}
	auto& executor = m_renderer.GetRenderExecutor();
	if (Config::GpuIndirectPredictsEmpty() && executor.IndirectDrawLikelyEmpty(args.args_addr)) {
		Common::PerfStats::Count(Common::PerfStats::Counter::IndirectDrawsCpuEmpty);
		return false;
	}
	if (indexed) {
		args.index_type_and_size = m_index_type_and_size;
		args.index_base_addr     = m_index_base_addr;
		args.index_buffer_size   = m_index_buffer_size;
	}

	// The shader takes the offsets from the draw parameters; the CPU path would write the
	// record's values here. Zero keeps the user-data snapshot independent of GPU results.
	for (const auto location: {registers.vertex_offset, registers.instance_offset}) {
		if (location != Pm4::SH_NOP) {
			g_hw_sh_indirect_func[location](*this, location, 0);
		}
	}
	if (!executor.DrawIndirect(m_submit_id, CurrentBuffer(), args)) {
		return false;
	}
	m_pending_instances = {.records    = args.args_addr,
	                       .stride     = stride_in_bytes,
	                       .max_count  = max_count,
	                       .count_addr = count_addr};
	return true;
}

uint32_t CommandProcessor::NumInstances() {
	if (m_pending_instances.records != 0) {
		const auto pending  = m_pending_instances;
		m_pending_instances = {};
		uint32_t count      = pending.max_count;
		if (pending.count_addr != nullptr) {
			count = std::min(static_cast<uint32_t>(*pending.count_addr), pending.max_count);
		}
		if (count != 0) {
			// instanceCount is the second word of both record layouts.
			const auto* last = reinterpret_cast<const volatile uint32_t*>(
			    pending.records + uint64_t {count - 1u} * pending.stride);
			m_num_instances = last[1]; // As the CPU path leaves it.
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
				LOGF("GPU indirect draw: a later draw reads NUM_INSTANCES (%u) back\n",
				     m_num_instances);
			}
		}
	}
	return m_num_instances;
}

} // namespace Libs::Graphics
