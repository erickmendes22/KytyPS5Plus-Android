#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

// A timed render pass: its size and how many draws / indices it held.
struct GpuPassInfo {
	uint32_t width   = 0;
	uint32_t height  = 0;
	uint32_t colors  = 0;
	bool     depth   = false;
	uint32_t draws   = 0;
	uint64_t indices = 0;
	// A second shader hash shown after the label's (per-draw marks: the vertex shader).
	uint64_t aux = 0;
};

// GPU execution time from Vulkan timestamps (--perf-stats only; inert otherwise). Every command
// buffer gets a timestamp at its start and end, which yields the true GPU busy time; Mark()
// closes a region (a render pass, a dispatch) and charges the time since the previous timestamp
// to its label. Results are read without waiting once the command buffer's tick has completed.
// Every kDumpSeconds the costliest labels go to _perf_gpu<instance>.txt and the top few to the log.
// Instance 0 also runs a GPU clock probe (KYTY_GPU_PROBE_MS, default every 2000 ms, 0 = off): a
// fixed ALU dispatch, fenced off from other work, at the start of a command buffer. Its GPU time
// goes to the log as "[perf-probe] t=<s> gpu_ms=<ms>" and tracks the shader clock, unlike game
// regions whose cost changes with the scene.
class GpuTimer {
public:
	GpuTimer(GraphicContext& graphics, MasterSemaphore& master);
	~GpuTimer();
	KYTY_CLASS_NO_COPY(GpuTimer);

	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }

	// Recording thread only: the command buffer was begun / is about to end / was submitted.
	void Begin(vk::CommandBuffer buffer);
	void Mark(vk::CommandBuffer buffer, const char* kind, uint64_t hash, const GpuPassInfo& pass = {});
	void End(vk::CommandBuffer buffer);
	void Submitted(uint64_t tick);
	// Reads finished command buffers' timestamps.
	void Collect();
	// After a device loss (GpuFatal hook): the command buffers still in flight, each with how far
	// its timestamps got, so the region the GPU was in when it hung or faulted is named.
	void DumpInFlight(vk::Result result);

	// The probe's push constants (gpu_clock_probe.comp).
	struct ProbeParams {
		uint64_t sink       = 0;
		uint32_t iterations = 0;
		float    never      = 0.0f;
	};

private:
	struct Label {
		const char* kind = nullptr;
		uint64_t    hash = 0;
		GpuPassInfo pass;
	};
	struct Recording {
		vk::QueryPool      pool  = nullptr;
		uint32_t           count = 0;
		uint64_t           tick  = 0;
		std::vector<Label> labels; // labels[i] closes the region ending at query i
		uint32_t           crumb_slot = 0;
	};
	struct Cost {
		uint64_t ns      = 0;
		uint64_t count   = 0;
		uint64_t draws   = 0;
		uint64_t indices = 0;
	};

	struct PendingProbe {
		uint32_t slot = 0;
		uint64_t tick = 0;
	};

	static constexpr uint32_t kQueriesPerBuffer = 1024;
	static constexpr int      kDumpSeconds      = 5;
	static constexpr uint32_t kProbeSlots       = 8; // two queries each

	void CreateProbe();
	// KYTY_GPU_BREADCRUMBS=1: after every region recorded outside a render pass (pass ends,
	// dispatches) the GPU writes the region's index to host-visible memory, fenced by full
	// barriers (slow; diagnostics only). DumpInFlight reads them back after a device loss.
	// KYTY_CRUMB_KINDS=<begin,pass,cs,mesh-cs,indirect-fixup>: only those regions get the fenced
	// write (barrier bisection); the others are not recorded. Default: all of them.
	void CreateBreadcrumbs();
	void WriteBreadcrumb(vk::CommandBuffer buffer, uint32_t value, bool fenced = true);
	[[nodiscard]] bool CrumbKind(const char* kind) const;
	void RecordProbe(vk::CommandBuffer buffer);
	void CollectProbes();

	// A free pool and how many of its queries need a reset.
	std::pair<vk::QueryPool, uint32_t> AcquirePool();
	void          Account(const Recording& recording, const std::vector<uint64_t>& stamps);
	void          MaybeDump();

	GraphicContext&            m_graphics;
	MasterSemaphore&           m_master;
	bool                       m_enabled     = false;
	double                     m_period_ns   = 1.0;
	uint64_t                   m_valid_mask  = ~uint64_t {0};
	bool                       m_recording   = false;
	Recording                  m_current;
	std::deque<Recording>      m_pending;
	std::mutex                 m_pending_mutex; // m_pending against DumpInFlight's thread
	std::vector<std::pair<vk::QueryPool, uint32_t>> m_free_pools; // pool, queries written last use
	std::vector<vk::QueryPool> m_all_pools;
	std::map<std::string, Cost> m_costs;
	uint64_t                   m_window_busy_ns = 0;
	int64_t                    m_last_dump_ns   = 0;
	uint32_t                   m_instance       = 0;
	vk::PipelineLayout         m_probe_layout   = nullptr;
	vk::Pipeline               m_probe_pipeline = nullptr;
	vk::QueryPool              m_probe_pool     = nullptr;
	int64_t                    m_probe_interval_ns = 0;
	uint32_t                   m_probe_iterations  = 0;
	uint32_t                   m_probe_groups      = 0;
	int64_t                    m_last_probe_ns     = 0;
	int64_t                    m_start_ns          = 0;
	uint32_t                   m_probe_next_slot   = 0;
	uint32_t                   m_probe_recorded_slot = 0;
	bool                       m_probe_recorded    = false; // recorded, not yet submitted
	std::deque<PendingProbe>   m_probe_pending;
	static constexpr uint32_t  kCrumbSlots = 256;
	vk::Buffer                 m_crumb_buffer = nullptr;
	vk::DeviceMemory           m_crumb_memory = nullptr;
	volatile uint32_t*         m_crumbs       = nullptr;
	uint32_t                   m_crumb_serial = 0;
	uint32_t                   m_crumb_kinds  = ~0u; // bit per kind, see CrumbKind
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_
