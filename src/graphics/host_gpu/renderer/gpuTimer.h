#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

// GPU execution time from Vulkan timestamps (--perf-stats only; inert otherwise). Every command
// buffer gets a timestamp at its start and end, which yields the true GPU busy time; Mark()
// closes a region (a render pass, a dispatch) and charges the time since the previous timestamp
// to its label. Results are read without waiting once the command buffer's tick has completed.
// Every kDumpSeconds the costliest labels go to _perf_gpu<instance>.txt and the top few to the log.
class GpuTimer {
public:
	GpuTimer(GraphicContext& graphics, MasterSemaphore& master);
	~GpuTimer();
	KYTY_CLASS_NO_COPY(GpuTimer);

	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }

	// Recording thread only: the command buffer was begun / is about to end / was submitted.
	void Begin(vk::CommandBuffer buffer);
	void Mark(vk::CommandBuffer buffer, const char* kind, uint64_t hash, uint32_t width = 0,
	          uint32_t height = 0);
	void End(vk::CommandBuffer buffer);
	void Submitted(uint64_t tick);
	// Reads finished command buffers' timestamps.
	void Collect();

private:
	struct Label {
		const char* kind   = nullptr;
		uint64_t    hash   = 0;
		uint32_t    width  = 0;
		uint32_t    height = 0;
	};
	struct Recording {
		vk::QueryPool      pool  = nullptr;
		uint32_t           count = 0;
		uint64_t           tick  = 0;
		std::vector<Label> labels; // labels[i] closes the region ending at query i
	};
	struct Cost {
		uint64_t ns    = 0;
		uint64_t count = 0;
	};

	static constexpr uint32_t kQueriesPerBuffer = 1024;
	static constexpr int      kDumpSeconds      = 5;

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
	std::vector<std::pair<vk::QueryPool, uint32_t>> m_free_pools; // pool, queries written last use
	std::vector<vk::QueryPool> m_all_pools;
	std::map<std::string, Cost> m_costs;
	uint64_t                   m_window_busy_ns = 0;
	int64_t                    m_last_dump_ns   = 0;
	uint32_t                   m_instance       = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_
