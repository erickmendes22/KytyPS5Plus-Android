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
	GpuPipeDrain,   // KYTY_GPU_PIPELINE: the front waiting for the record thread to drain
	GpuPipeFull,    // ... the front waiting for a free slot in the record queue
	GpuPipeRecord,  // ... the record thread running jobs (busy time, not a stall)
	GpuPipeWalk,    // ... the front's SRT walk / program lookup of queued items (busy time)
	GpuPipeCopy,    // ... the front's register and resource copies for queued items (busy time)
	GpuFanWait,     // KYTY_GPU_FAN: the recorder waiting for (or running) a walk job
	GpuFanWorker,   // ... walk-pool time of the jobs the recorder took from a worker (busy time)
	GpuFanLookup,   // ... the front's program-entry lookup for walk jobs (shared lock or fan_memo)
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
	VideoUploadRaces, // video texture uploads while AvPlayer was still writing that buffer
	VideoFrameRequests, // sceAvPlayerGetVideoData calls while playing
	VideoFrameDelivered,
	VideoFrameStarved,  // ...that found the decoded-frame queue empty
	VideoBlackUploads,  // video texture uploads whose sampled luma was all black
	VideoUploads,       // texture uploads sourced from an AvPlayer video buffer
	VideoOutRefreshUploads, // display buffers re-uploaded from guest memory before a flip
	BlankPresents,      // swapchain presents with no (or a blank) main-bus frame
	// Why render passes with draws ended (CommandBuffer::EndRendering reasons).
	PassEndState,       // next draw targets different attachments
	PassEndTransit,     // image layout transition (barrier) for a sampled/stored image
	PassEndAttach,      // layout transition of a color/depth attachment being acquired
	PassEndImageCopy,   // image upload/download/copy/blit/resolve
	PassEndBuffer,      // buffer cache / stream buffer upload or download
	PassEndCompute,     // compute dispatch (incl. mesh ring reuse)
	PassEndShaderWrite, // shader buffer writes or GDS barriers
	PassEndSubmit,      // command buffer submitted
	PassEndOther,
	HoistedBarriers,    // image barriers recorded ahead of the open render pass instead of ending it
	GraphicsComputeBarriers, // render passes ordered before all later work (KYTY_PASS_BARRIER)
	PassEndAttachSame,  // ...of PassEndAttach: same attachment images, only their layout changed
	RenderScaleUpBlits,   // --render-scale: images blitted from guest size to the scaled size
	RenderScaleDownBlits, // --render-scale: images blitted back to guest size
	// Write-tracking faults handled, by thread and access (GPU thread = command processor).
	FaultsGpuRead,
	FaultsGpuWrite,
	FaultsGuestRead,
	FaultsGuestWrite,
	IdleSubmits, // --idle-submit: command buffers submitted early because the GPU ran dry
	// Texture cache eviction (collector and evict-for-allocation): images, their bytes, and how
	// many of them were GPU-written and downloaded to guest memory first.
	GcEvictions,
	GcEvictedBytes,
	GcDownloads,
	// Indirect draw packets (per draw record) executed on the GPU (--gpu-indirect) or with a CPU
	// read of their arguments.
	IndirectDrawsGpu,
	IndirectDrawsCpu,
	IndirectDrawsCpuEmpty, // ...of IndirectDrawsCpu: predicted empty (see IndirectDrawFixup)
	// Two-stage GPU thread sizing (graphics/host_gpu/renderer/pipelineProbe.h).
	PipeItems,             // draws and dispatches prepared
	PipeDmaItems,          // ...whose programs write memory through raw device addresses
	PipeSrtReads,          // SRT walker host reads
	PipeDrainSrtWrite,     // walker read of memory an unrecorded item in the window writes
	PipeDrainSrtDma,       // walker read while an unrecorded item in the window may write via BDA
	PipeDrainSrtDirty,     // walker read of GPU-dirty memory (in-place readback)
	PipeDrainSync,         // synchronous command from a guest thread
	PipeDrainIndirect,     // indirect draw arguments read on the CPU
	PipeWaitRegMemBlocked, // WAIT_REG_MEM not satisfied when parsed (the ring suspends)
	PipeRecordRegAccess,   // live register block accesses while recording
	PipeResourceBytes,     // materialized resource bytes per item (the per-item copy)
	Count
};

// An extra section appended to the "[perf-wait]" line (the renderer's walk fan-out and pipeline
// drain counters), called once per report with the frames-per-report factor.
using ReportSection = void (*)(std::string& out, double per_frame);
void SetReportSection(ReportSection section);

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

// --- Readbacks of GPU-written guest memory (BufferCache::ReadMemory), by reader ---
// How a synchronous readback got its bytes.
enum class ReadbackMode : uint8_t {
	CopyFlush,     // copy recorded in the open command buffer, which is submitted and waited for
	InPlaceDone,   // read from the host-visible buffer; the writer had already completed
	InPlaceWait,   // read in place after waiting for the writer's (older) command buffer
	InPlaceFlush,  // read in place after submitting the open command buffer that wrote it
	Count
};
// The PM4 opcode the command processor is executing on this thread (0 = none), so readbacks
// taken by the GPU thread can be attributed to the packet that read the memory.
void               SetPm4Opcode(uint32_t opcode);
[[nodiscard]] uint32_t CurrentPm4Opcode();
// The shader whose resources the GPU thread is specializing (0 = none): "[perf-trace]" lines
// name it as ctx=, so a specialization readback can be attributed to its shader.
void               SetTraceShader(uint64_t hash);
[[nodiscard]] uint64_t CurrentTraceShader();
// What the specialization was evaluating (a static string, nullptr = unknown), as site=.
const char*        ExchangeTraceSite(const char* site);
[[nodiscard]] const char* CurrentTraceSite();
class ScopedTraceSite {
public:
	explicit ScopedTraceSite(const char* site): m_previous(ExchangeTraceSite(site)) {}
	~ScopedTraceSite() { ExchangeTraceSite(m_previous); }
	ScopedTraceSite(const ScopedTraceSite&)            = delete;
	ScopedTraceSite& operator=(const ScopedTraceSite&) = delete;

private:
	const char* m_previous;
};
// Reader tags for readbacks requested by other threads.
constexpr uint32_t kReaderGuestRead  = 0x1000;
constexpr uint32_t kReaderGuestWrite = 0x1001;
constexpr uint32_t kReaderOther      = 0x1002;
// `lag` = ticks between the open command buffer and the newest writer of the bytes (0 = the
// open one wrote them).
void RecordReadback(Wait site, uint32_t reader, uint64_t address, uint64_t bytes, uint64_t wait_ns,
                    uint64_t lag, ReadbackMode mode);
// KYTY_FAULT_TRACE=1 (with --perf-stats): "[perf-trace]" lines naming the host frames (module +
// offset, walked along the x29 frame-pointer chain from `fp`) behind GPU-thread readbacks and
// faults, at most a few dozen per tag. `fp` = 0 traces the caller's own frames.
[[nodiscard]] bool TraceEnabled();
void TraceHostStack(const char* tag, uint64_t address, uint64_t pc, uint64_t lr, uint64_t fp);

// The "[perf-readback]" line of the last period (empty when nothing was read back); resets it.
std::string TakeReadbackSummary(double seconds, uint64_t flips);

} // namespace Common::PerfStats

#endif /* KYTY_COMMON_PERFWAITS_H_ */
