#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Libs::Graphics {

struct GraphicContext;

// The renderer's timeline semaphore. Ticks are allocated by the recording thread (NextTick),
// signalled by vkQueueSubmit (MarkSubmitted, possibly on a submit thread) and reached by the GPU
// (KnownGpuTick). Waits for a tick first wait for its submission, so a queued-but-unsubmitted
// signal is never waited on in the driver.
class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t SubmittedTick() const noexcept {
		return m_submitted_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_semaphore; }

	// Query the driver for the GPU's progress. A no-op while the tracker thread runs: it keeps
	// KnownGpuTick() current without timeline queries on the working threads.
	void Refresh();
	// Block until the GPU reached `tick` (after its submission).
	void Wait(uint64_t tick);

	// The signal for `tick` (and every earlier one) has been handed to vkQueueSubmit.
	void MarkSubmitted(uint64_t tick);
	void WaitSubmitted(uint64_t tick);

	// --timeline-tracker: a thread waits for each submitted tick in turn and publishes it.
	void StartTracker();
	void StopTracker();
	[[nodiscard]] bool TrackerActive() const noexcept {
		return m_tracker_active.load(std::memory_order_acquire);
	}

private:
	void TrackerThread(const std::stop_token& stop);
	void Advance(uint64_t counter) noexcept;

	GraphicContext&         m_graphics;
	vk::Semaphore           m_semaphore = nullptr;
	std::atomic<uint64_t>   m_gpu_tick {0};
	std::atomic<uint64_t>   m_current_tick {1};
	std::atomic<uint64_t>   m_submitted_tick {0};
	std::mutex              m_submitted_mutex;
	std::condition_variable m_submitted_cv;
	std::atomic<bool>       m_tracker_active {false};
	std::jthread            m_tracker;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
