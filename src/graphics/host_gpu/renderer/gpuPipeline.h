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
	// Queued jobs and whether one is running (watchdog; any thread).
	[[nodiscard]] size_t Queued() const;
	[[nodiscard]] bool   Busy() const;

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
// The record thread if it was created, else nullptr (watchdog).
RecordThread* BackIfCreated();
// True on the record thread.
[[nodiscard]] bool OnBack();
// Why the front drains the back stage (--perf-stats "drains:" counts and wait per cause).
enum class DrainCause : uint8_t {
	SyncCommand,    // a guest thread's synchronous command (reads back what queued jobs record)
	Packet,         // a PM4 packet the front runs inline (per opcode: "drain_ops")
	Suspend,        // the submission suspends (WAIT_REG_MEM, ...)
	EndOfStream,    // ProcessPm4 returns
	InlineDraw,     // a draw that runs on the front (metadata, resolve, depth copy)
	InlineDispatch, // a dispatch that runs on the front
	Other,
	Count
};

// Drains the back stage when called on the front with jobs outstanding; no-op elsewhere.
// `opcode`: the PM4 opcode for DrainCause::Packet.
void DrainForFront(DrainCause cause = DrainCause::Other, uint32_t opcode = 0);

// Installs the "[perf-wait]" section with the drain and walk fan-out counters (once).
void InstallReportSection();

} // namespace Libs::Graphics::GpuPipeline

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINE_H_
