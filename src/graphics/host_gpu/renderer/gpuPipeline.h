#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPIPELINE_H_

#include "common/uniqueFunction.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <stop_token>
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

// KYTY_PIPE_BATCH=1 (default 0); with KYTY_LIVE_KNOBS=1 the live key pipe_batch=0/1 overrides it.
// Batched handoff: the record thread takes every queued job at once and the two threads only wake
// each other when the other one sleeps (a full queue, a drain, an idle record thread), instead of
// three condition-variable broadcasts per job (a futex syscall each on bionic). Read by the record
// thread before each batch.
[[nodiscard]] bool BatchEnabled();

// The record thread's job queue. Jobs run in order; Drain() returns once every job queued so far
// has finished. Bounded: Enqueue blocks while kMaxQueued jobs wait (with batched handoff: while
// kMaxQueued jobs are queued or taken but unfinished).
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
	static void* Entry(void* self);

	mutable std::mutex                       m_mutex;
	std::condition_variable                  m_work;
	std::condition_variable                  m_done;
	std::deque<Common::UniqueFunction<void>> m_jobs;
	bool                                     m_busy = false;
	// Batched handoff (BatchEnabled): the mode of the current batch, the jobs taken but not
	// finished, whether the record thread sleeps on m_work, and the front threads waiting on m_done.
	bool                                     m_batch         = false;
	uint32_t                                 m_running       = 0;
	bool                                     m_sleeping      = false;
	uint32_t                                 m_full_waiters  = 0;
	uint32_t                                 m_drain_waiters = 0;
	// The recorder re-walks SRTs (fallback, stolen fan-out jobs), which recurse deeply
	// (SrtWalker::EvaluateWide); bionic's default 1 MiB thread stack is not enough (walk workers:
	// d9b44d54). A pthread with kStackBytes (virtual; touched pages only).
	static constexpr size_t                  kStackBytes = 16u << 20u;
	std::stop_source                         m_stop;
	pthread_t                                m_thread {};
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
