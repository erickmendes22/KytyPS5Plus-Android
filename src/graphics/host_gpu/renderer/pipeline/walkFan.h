#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_WALKFAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_WALKFAN_H_

#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/host_gpu/renderer/pipeline/walkReader.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <thread>
#include <vector>

// Fan-out of the per-draw SRT walk (KYTY_GPU_FAN, research/gputhread_pipeline.md section 7).
//
// The front (the GPU-thread loop) parses PM4 and, for every stage of a queued draw or dispatch,
// submits a WalkJob: the program entry's resource plan, copies of the walk inputs and the current
// WalkReader::View. WalkPool workers run MaterializeResources on their own scratch into the job's
// own outputs, reading guest memory only through a WalkReader::WorkerWalk (C2: the backing alias,
// never a fault; GPU-modified or unbacked bytes are refused).
// The recorder (the record thread, which owns the program cache) takes the jobs in item order,
// checks each one with Accept() and moves an accepted result into the program entry; anything
// else is walked inline on the recorder exactly as before.
//
// Nothing here touches the renderer: the program cache, the command processor and BufferCache
// plug in through WalkContext and the job fields. Unit tests: tests/WalkFanTests.cpp.
namespace Libs::Graphics::WalkFan {

namespace IR = ShaderRecompiler::IR;

// What workers and the recorder share (installed once by the renderer): how a worker reaches
// guest memory (WalkReader::GuestMemory(): fold + the backing alias, never a fault) and the
// journal of GPU writes the recorder notes (BufferCache::SetWalkJournal), used at pickup.
struct WalkContext {
	WalkReader::Memory              memory;
	const WalkReader::WriteJournal* journal = nullptr;
};

// The installed context, or nullptr: without one the front submits no jobs (every walk inline).
void                             SetContext(const WalkContext* context);
[[nodiscard]] const WalkContext* Context();

// Why a walk ran inline on the recorder, or how a job ended (counters, --perf-stats "fan:").
enum class Outcome : uint8_t {
	Accepted,          // the job's result was used
	RefusedGpuModified, // a read touched bytes the GPU modified as of the job's view
	RefusedNoBacking,  // a read the backing alias cannot serve (flexible memory)
	RefusedInvalid,    // a read that does not fold to host memory
	Invalidated,       // GPU writes noted since the job's view overlap what the walk read
	Failed,            // MaterializeResources failed
	NoEntry,           // the front found no program entry (first use: the recorder translates)
	CallSource,        // a source that calls other code (its plan can change per draw)
	Memo,              // KYTY_SRT_MEMO is on (the memo walks on the recorder)
	NoContext,         // no WalkContext installed
	Mismatch,          // the recorder's request did not match the front's (other entry or stage)
	Rerequest,         // a second request for the same draw (mesh path switch, packed varyings)
	Reused,            // ...of an entry this item already materialized: no walk at all
	Count
};

struct WalkJob {
	// Inputs (front).
	const IR::ResourcePlan*                plan  = nullptr;
	const void*                            entry = nullptr; // identity of the plan's program entry
	uint32_t                               stage = 0;
	std::vector<uint32_t>                  user_data;
	uint64_t                               shader_base           = 0;
	std::array<uint32_t, 3>                workgroup_counts      {};
	bool                                   has_workgroup_counts  = false;
	uint64_t                               indirect_args_address = 0;
	IR::SrtAddressProbe                    mapped                = nullptr; // thread-safe probe
	std::shared_ptr<const WalkReader::View> view; // the GPU-modified bytes as of submission

	// Outputs (the walking thread; read after TakeOrWait()).
	IR::ResourceSnapshot       resources;
	IR::ResourceSpecialization specialization;
	WalkReader::Reads          reads;
	uint64_t                   view_generation = 0;
	bool                       ok              = false;
	uint64_t                   walk_ns         = 0;

	// Runs the walk on the calling thread (a worker, or the recorder taking it).
	void Run(const WalkContext& context);
	// Recorder: runs the job here if no worker has started it, else waits for it. True when this
	// thread ran it.
	bool TakeOrWait(const WalkContext& context);
	[[nodiscard]] bool Done() const { return m_state.load(std::memory_order_acquire) == State::Done; }

private:
	friend class WalkPool;
	enum class State : uint8_t { Queued, Running, Done };
	bool TryClaim();
	void Finish();

	std::atomic<State>      m_state {State::Queued};
	std::mutex              m_mutex;
	std::condition_variable m_done;
};

// The recorder's verdict on a finished job (the pickup rule): not ok -> Failed; any refused read
// -> Refused* by its first reason (even when no range was recorded); otherwise Accepted when no GPU
// write noted in the journal after the job's view intersects a range it read
// (WalkReader::StillValid), else Invalidated. No journal -> Invalidated.
[[nodiscard]] Outcome Accept(const WalkJob& job, const WalkContext& context);

// The jobs of one queued item, in the order the front requested its stages. The recorder takes
// them in the same order.
class PrewalkList {
public:
	void Push(std::shared_ptr<WalkJob> job, Outcome inline_reason = Outcome::Count);
	// Front: a job to submit when the item's requests are done (one pool lock per item, SubmitPending).
	void PushPending(std::shared_ptr<WalkJob> job) {
		m_pending.push_back(job);
		Push(std::move(job));
	}
	// Front: hands the item's pending jobs to the pool in one batch.
	void SubmitPending();
	// Front: the walk view for every job of this item (taken once per item).
	std::shared_ptr<const WalkReader::View> view;
	// The recorder's next request: the job (or nullptr with the reason it is inline).
	std::shared_ptr<WalkJob> Next(Outcome& reason);
	[[nodiscard]] size_t Size() const { return m_jobs.size(); }
	// Entries this item materialized (recorder): a re-request of one of them for the same draw
	// has the same inputs, so its resources are already in the entry.
	void NoteWalked(const void* entry) {
		if (m_walked_count < m_walked.size()) m_walked[m_walked_count++] = entry;
	}
	[[nodiscard]] bool Walked(const void* entry) const {
		for (size_t i = 0; i < m_walked_count; i++) {
			if (m_walked[i] == entry) return true;
		}
		return false;
	}

private:
	std::vector<std::shared_ptr<WalkJob>> m_pending;
	std::array<const void*, 8> m_walked {};
	size_t                     m_walked_count = 0;
	struct Slot {
		std::shared_ptr<WalkJob> job;
		Outcome                  reason = Outcome::Count;
	};
	std::vector<Slot> m_jobs;
	size_t            m_next = 0;
};

// The front's sink while it requests the programs of an item (ProgramCache::Get submits jobs
// instead of walking), and the recorder's list while it records that item.
inline thread_local PrewalkList* t_walk_sink = nullptr;
inline thread_local PrewalkList* t_prewalk   = nullptr;

class ScopedList {
public:
	ScopedList(PrewalkList*& slot, PrewalkList* list): m_slot(slot), m_previous(slot), m_list(list) { m_slot = list; }
	~ScopedList() {
		if (m_list != nullptr) m_list->SubmitPending(); // the front's sink: submit the item's jobs
		m_slot = m_previous;
	}
	ScopedList(const ScopedList&)            = delete;
	ScopedList& operator=(const ScopedList&) = delete;

private:
	PrewalkList*& m_slot;
	PrewalkList*  m_previous;
	PrewalkList*  m_list;
};

// The front's lookup memo (live fan_memo=1): request key -> program entry, so a repeated
// request skips the shared lock and hash-map probe of ProgramCache::SubmitWalk. Valid because
// entries are never erased while running and the stored key is compared in full on every hit;
// a miss is not remembered (the entry may be created later). Direct-mapped, front thread only.
template <typename Key, typename Value, size_t Slots = 1024>
class LookupMemo {
public:
	static_assert((Slots & (Slots - 1)) == 0, "power of two");
	[[nodiscard]] Value* Find(const Key& key, size_t hash) {
		auto& slot = m_slots[hash & (Slots - 1)];
		if (slot.value != nullptr && slot.hash == hash && slot.key == key) {
			m_hits++;
			return slot.value;
		}
		m_misses++;
		return nullptr;
	}
	void Insert(const Key& key, size_t hash, Value* value) {
		auto& slot = m_slots[hash & (Slots - 1)];
		slot.key   = key;
		slot.hash  = hash;
		slot.value = value;
	}
	void Clear() {
		for (auto& slot: m_slots) slot.value = nullptr;
	}
	[[nodiscard]] uint64_t Hits() const { return m_hits; }
	[[nodiscard]] uint64_t Misses() const { return m_misses; }

private:
	struct Slot {
		Key    key {};
		size_t hash  = 0;
		Value* value = nullptr;
	};
	std::array<Slot, Slots> m_slots {};
	uint64_t                m_hits   = 0;
	uint64_t                m_misses = 0;
};

// Worker threads walking submitted jobs in submission order.
class WalkPool {
public:
	explicit WalkPool(uint32_t workers);
	~WalkPool();
	WalkPool(const WalkPool&)            = delete;
	WalkPool& operator=(const WalkPool&) = delete;

	void Submit(std::shared_ptr<WalkJob> job);
	// One lock and one wake-up for an item's jobs.
	void SubmitBatch(std::vector<std::shared_ptr<WalkJob>>& jobs);
	// Keeps `workers` threads (1..kMaxWorkers); call between items.
	void Resize(uint32_t workers);
	[[nodiscard]] uint32_t Workers() const;
	// Queued and running jobs (watchdog; any thread).
	[[nodiscard]] size_t   Queued() const;
	[[nodiscard]] uint32_t Running() const;
	// Waits until every submitted job has finished (shutdown, tests).
	void Quiesce();

	static constexpr uint32_t kMaxWorkers = 4;

private:
	void Run(uint32_t index, const std::stop_token& stop);

	mutable std::mutex                    m_mutex;
	std::condition_variable_any           m_work;
	std::condition_variable               m_idle;
	std::deque<std::shared_ptr<WalkJob>>  m_queue;
	uint32_t                              m_running = 0;
	// A worker thread with its own stack size: SRT walks recurse deeply (EvaluateWide), and the
	// default 1 MiB of a bionic thread overflowed on Astro Bot's intro content (c14_fanlive1).
	class Worker {
	public:
		Worker(WalkPool& pool, uint32_t index);
		~Worker(); // requests stop and joins
		Worker(const Worker&)            = delete;
		Worker& operator=(const Worker&) = delete;
		void RequestStop() { m_stop.request_stop(); }

	private:
		static void* Entry(void* self);
		WalkPool&         m_pool;
		uint32_t          m_index;
		std::stop_source  m_stop;
		pthread_t         m_thread {};
	};
	static constexpr size_t kWorkerStackBytes = 16u << 20u;
	std::vector<std::unique_ptr<Worker>>  m_threads; // Resize's caller only
	std::atomic<uint32_t>                 m_workers {0};
};

// The renderer's view publisher (the recorder publishes BufferCache's GPU-modified ranges at item
// boundaries; the front hands Current() to each job) and write journal (BufferCache notes its GPU
// writes; NoteBackingWrite the cache writes into guest memory's backing).
WalkReader::ViewPublisher& Publisher();
WalkReader::WriteJournal&  Journal();
// Marks the journal live (once the fan-out was first enabled); before that the hooks are no-ops.
void SetJournalActive();
// A recorder-side write into guest memory that is not a BufferCache GPU-modify (downloads,
// texture write-backs): noted so walks that read the old bytes are invalidated. Host-folded.
void NoteBackingWrite(uint64_t host_address, uint64_t size);

// The process-wide pool (created on first use with KYTY_GPU_FAN_WORKERS / live fan_workers,
// default 2).
WalkPool& Pool();

// KYTY_GPU_FAN=0/1/auto (default auto on Android, 0 elsewhere) or live gpu_fan=0/1; read by the front at submission boundaries.
// KYTY_GPU_FAN=auto (or KYTY_FAN_AUTO=1): on while the mean SRT walk costs >= 25 us, off once it
// falls to <= 18 us (KYTY_FAN_AUTO_ON_US / _OFF_US), per 2 s window with a 6 s dwell
// (fanAutoPolicy.h); every window and switch is logged as "[fan-auto]". The fan-out also turns
// the two-stage pipeline on, so auto decides both. Calls from the front only (it updates the policy).
[[nodiscard]] bool Enabled();
// KYTY_GPU_FAN=auto is in effect.
[[nodiscard]] bool AutoMode();
// Live fan_memo=1 (default 0): ProgramCache::SubmitWalk remembers request key -> entry (LookupMemo).
[[nodiscard]] bool MemoEnabled();
// Live fan_shadow=1: the recorder also walks every accepted job itself and compares.
[[nodiscard]] bool Shadow();
// Applies live fan_workers to the pool (front, at submission boundaries).
void PollWorkers();

// Counters (--perf-stats "fan:" section; also read by tests).
void     Count(Outcome outcome);
void     CountSubmitted();
void     CountStolen();
void     CountShadowMismatch();
uint64_t TakeCount(Outcome outcome);
uint64_t TakeSubmitted();
uint64_t TakeStolen();
uint64_t TakeShadowMismatch();

// Field-by-field equality of two materializations (fan_shadow).
[[nodiscard]] bool SameResult(const IR::ResourceSnapshot& a, const IR::ResourceSpecialization& as,
                              const IR::ResourceSnapshot& b, const IR::ResourceSpecialization& bs);

} // namespace Libs::Graphics::WalkFan

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_WALKFAN_H_
