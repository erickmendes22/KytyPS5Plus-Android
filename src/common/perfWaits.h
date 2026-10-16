#ifndef KYTY_COMMON_PERFWAITS_H_
#define KYTY_COMMON_PERFWAITS_H_

// Where frames wait (--perf-stats). Timed blocking points of the GPU (command processor) thread
// and of guest threads, host waits on the renderer's timeline semaphore by call site, renderer
// work counters and the true GPU busy time measured with Vulkan timestamps. Reported once a
// second as a "[perf-wait]" line in milliseconds per presented frame. Every entry point is one
// predictable branch while --perf-stats is off.

#include "common/perfStats.h"

#include <cstdint>
#include <string>

namespace Common::PerfStats {

enum class Wait : uint8_t {
	// GPU (command processor) thread
	GpuIdle,        // no guest submission queued
	GpuBlocked,     // every queued submission blocked (WAIT_REG_MEM, CE/DE counters, ...)
	GpuFlipDone,    // PM4 WAIT_FLIP_DONE: until a display buffer leaves the flip queue
	GpuFlipSlot,    // flip queue full: until the presenter frees a slot
	GpuSyncCommand, // executing a synchronous command a guest thread sent (fault readbacks)
	GpuFault,       // a write-tracking fault taken by the GPU thread itself (CPU reads of GPU data)
	QueueSubmit,    // vkQueueSubmit
	GpuProcess,     // wall time processing guest submissions (includes the waits inside)
	GpuPriority,    // waiting for the completion thread to run guest-memory callbacks
	TimelineQuery,  // vkGetSemaphoreCounterValue on the renderer timeline (any thread)
	// vkWaitSemaphores on the renderer timeline, by call site (any thread)
	SemFinish,
	SemFlush,
	SemStreamBuffer,
	SemMeshRing,
	SemDownload,     // synchronous readback of GPU-written buffer memory (other callers)
	SemReadFault,    // ... for a CPU read fault
	SemWriteFault,   // ... for a CPU write fault
	SemIndirectArgs, // ... for indirect dispatch arguments read on the CPU
	SemMetadata,     // ... for color metadata read on the CPU
	SemGc,
	SemFaultBuffer,
	SemSwapchain,
	SemDescriptors,
	SemPriority, // background completion thread (label writes): not a stall by itself
	SemOther,
	// guest threads
	GuestFault,        // handling a write-tracking fault on GPU-tracked memory
	GuestSyncCommand,  // waiting for the GPU thread to run a synchronous command
	GuestSuspendPoint, // AgcSuspendPoint: previous frame's GPU work must complete
	GuestFlipDone,     // waiting for a flip to complete
	GuestIdle,         // GuestGpu::WaitForIdle
	Count
};

enum class Counter : uint8_t {
	Submits,
	RenderPasses,     // vkCmdBeginRendering
	Draws,
	Dispatches,
	MeshDispatches,   // emulated mesh stage compute dispatches
	MeshIndices,      // indices drawn by emulated mesh draws
	UploadBytes,      // CPU -> GPU buffer copies
	ImageUploadBytes, // CPU -> GPU image copies
	DownloadBytes,    // GPU -> CPU buffer copies
	SyncDownloads,    // downloads that wait for the GPU before returning
	ImageDownloadBytes,
	GpuBusyNs,        // command buffer execution time (Vulkan timestamps)
	GpuSubmitsTimed,
	Count
};

namespace Detail {
extern std::atomic<uint64_t> g_wait_ns[static_cast<size_t>(Wait::Count)];
extern std::atomic<uint64_t> g_wait_count[static_cast<size_t>(Wait::Count)];
extern std::atomic<uint64_t> g_counters[static_cast<size_t>(Counter::Count)];
int64_t                      MonotonicNs();
// Blocking waits at least this long are logged one by one ("[perf-event]") to find hitches.
constexpr uint64_t kLongWaitNs = 20'000'000;
void               LogLongWait(Wait kind, int64_t start_ns, uint64_t ns);
} // namespace Detail

inline void AddWait(Wait kind, uint64_t ns) {
	if (Enabled()) {
		Detail::g_wait_ns[static_cast<size_t>(kind)].fetch_add(ns, std::memory_order_relaxed);
		Detail::g_wait_count[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
	}
}

inline void Count(Counter kind, uint64_t n = 1) {
	if (Enabled()) {
		Detail::g_counters[static_cast<size_t>(kind)].fetch_add(n, std::memory_order_relaxed);
	}
}

class ScopedWait {
public:
	explicit ScopedWait(Wait kind): kind_(kind), start_ns_(Enabled() ? Detail::MonotonicNs() : 0) {}
	~ScopedWait() {
		if (start_ns_ != 0) {
			const auto ns = static_cast<uint64_t>(Detail::MonotonicNs() - start_ns_);
			AddWait(kind_, ns);
			if (ns >= Detail::kLongWaitNs) {
				Detail::LogLongWait(kind_, start_ns_, ns);
			}
		}
	}
	ScopedWait(const ScopedWait&)            = delete;
	ScopedWait& operator=(const ScopedWait&) = delete;

private:
	Wait    kind_;
	int64_t start_ns_;
};

// Names the call site of the timeline-semaphore waits issued on this thread while in scope.
class ScopedWaitSite {
public:
	explicit ScopedWaitSite(Wait site);
	~ScopedWaitSite();
	ScopedWaitSite(const ScopedWaitSite&)            = delete;
	ScopedWaitSite& operator=(const ScopedWaitSite&) = delete;

private:
	Wait previous_;
};

[[nodiscard]] Wait CurrentWaitSite();

// Marks the calling thread as the GPU (command processor) thread for fault attribution.
void SetGpuThread();
[[nodiscard]] bool IsGpuThread();

// Writes one report line to the perf log (logcat tag KytyPerf on Android, stderr elsewhere) and
// to _perf_log.txt.
void EmitLine(const std::string& line);

// The "[perf-wait]" report of the last period, per presented frame; resets the period.
std::string TakeWaitSummary(double seconds, uint64_t flips);

} // namespace Common::PerfStats

#endif /* KYTY_COMMON_PERFWAITS_H_ */
