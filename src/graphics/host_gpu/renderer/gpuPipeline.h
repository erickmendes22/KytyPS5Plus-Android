#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINE_H_

#include "common/uniqueFunction.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

// Two-stage GPU thread (KYTY_GPU_PIPELINE, research/gputhread_pipeline.md). The GPU thread (the
// "front") keeps parsing PM4 and runs the SRT walk / materialize / program lookup of a draw or
// dispatch; the bind + record part of that item runs as a job on a record thread (the "back"),
// in submission order, while the front prepares the next item. Everything else the front does
// that touches renderer state (caches, the command scheduler, Vulkan, guest-memory readbacks)
// first drains the back stage, so renderer state keeps a single owner at any time:
//   - the back thread while it runs jobs,
//   - the front thread once the queue is drained.
// The back thread is a GPU thread for GuestGpu::IsGpuThread() (faults it takes are handled
// inline, as on the front).
namespace Libs::Graphics::GpuPipeline {

// KYTY_GPU_PIPELINE=1 (default 0); with KYTY_LIVE_KNOBS=1 the live key gpu_pipe=0/1 overrides it.
// Read by the front at item boundaries.
[[nodiscard]] bool Enabled();

// The record thread's job queue. Jobs run in order; Drain() returns once every job queued so far
// has finished. Bounded: Enqueue blocks while kMaxQueued jobs wait.
class RecordThread {
public:
	static constexpr uint32_t kMaxQueued = 64;

	RecordThread();
	~RecordThread();
	RecordThread(const RecordThread&)            = delete;
	RecordThread& operator=(const RecordThread&) = delete;

	void Enqueue(Common::UniqueFunction<void>&& job);
	void Drain();
	[[nodiscard]] bool Idle() const;

private:
	void Run(const std::stop_token& stop);

	mutable std::mutex                       m_mutex;
	std::condition_variable                  m_work;
	std::condition_variable                  m_done;
	std::deque<Common::UniqueFunction<void>> m_jobs;
	bool                                     m_busy = false;
	std::jthread                             m_thread;
};

// The process-wide record thread (created on first use by the front).
RecordThread& Back();
// True on the record thread.
[[nodiscard]] bool OnBack();
// Drains the back stage when called on the front with jobs outstanding; no-op elsewhere.
void DrainForFront();

} // namespace Libs::Graphics::GpuPipeline

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINE_H_
