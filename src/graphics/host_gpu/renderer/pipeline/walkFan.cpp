#include "graphics/host_gpu/renderer/pipeline/walkFan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace Libs::Graphics::WalkFan {

namespace {

std::atomic<const WalkContext*> g_context {nullptr};

std::array<std::atomic<uint64_t>, static_cast<size_t>(Outcome::Count)> g_outcomes {};
std::atomic<uint64_t> g_submitted {0};
std::atomic<uint64_t> g_stolen {0};
std::atomic<uint64_t> g_shadow_mismatch {0};

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// Per-thread walk state, reused across jobs (the vectors keep their capacity).
IR::ResourcePlan::Scratch& ThreadScratch() {
	thread_local IR::ResourcePlan::Scratch scratch;
	return scratch;
}

} // namespace

void SetContext(const WalkContext* context) {
	g_context.store(context, std::memory_order_release);
}

const WalkContext* Context() {
	return g_context.load(std::memory_order_acquire);
}

void WalkJob::Run(const WalkContext& context) {
	const auto start = NowNs();
	IR::SrtRuntime base {
	    .user_data             = user_data,
	    .shader_base           = shader_base,
	    .mapped                = mapped,
	    .indirect_args_address = indirect_args_address,
	};
	if (has_workgroup_counts) base.workgroup_counts = workgroup_counts;
	WalkReader::WorkerWalk walk(view, context.memory);
	{
		const auto guard = walk.ThreadReader();
		ok = IR::MaterializeResources(*plan, ThreadScratch(), walk.Runtime(base), resources, specialization);
	}
	view_generation = walk.Generation();
	reads           = walk.TakeReads();
	walk_ns         = NowNs() - start;
	Finish();
}

bool WalkJob::TryClaim() {
	auto expected = State::Queued;
	return m_state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel);
}

void WalkJob::Finish() {
	{
		std::lock_guard lock(m_mutex);
		m_state.store(State::Done, std::memory_order_release);
	}
	m_done.notify_all();
}

bool WalkJob::TakeOrWait(const WalkContext& context) {
	if (TryClaim()) {
		Run(context);
		return true;
	}
	if (!Done()) {
		std::unique_lock lock(m_mutex);
		m_done.wait(lock, [this] { return m_state.load(std::memory_order_acquire) == State::Done; });
	}
	return false;
}

Outcome Accept(const WalkJob& job, const WalkContext& context) {
	// A refused read also fails the walk: report the refusal, not the failure.
	if (job.reads.Refused()) {
		switch (job.reads.first_refusal) {
			case WalkReader::Reads::Refusal::GpuModified: return Outcome::RefusedGpuModified;
			case WalkReader::Reads::Refusal::NoBacking: return Outcome::RefusedNoBacking;
			default: return Outcome::RefusedInvalid;
		}
	}
	if (!job.ok) return Outcome::Failed;
	if (context.journal == nullptr) return Outcome::Invalidated;
	return WalkReader::StillValid(job.reads, job.view_generation, *context.journal) ? Outcome::Accepted
	                                                                                : Outcome::Invalidated;
}

void PrewalkList::Push(std::shared_ptr<WalkJob> job, Outcome inline_reason) {
	m_jobs.push_back({std::move(job), inline_reason});
}

std::shared_ptr<WalkJob> PrewalkList::Next(Outcome& reason) {
	if (m_next >= m_jobs.size()) {
		reason = Outcome::Rerequest;
		return nullptr;
	}
	auto& slot = m_jobs[m_next++];
	reason     = slot.reason;
	return std::move(slot.job);
}

WalkPool::WalkPool(uint32_t workers) {
	Resize(workers);
}

WalkPool::~WalkPool() {
	Quiesce();
	for (auto& thread: m_threads) thread.request_stop();
	m_work.notify_all();
	m_threads.clear(); // joins
}

void WalkPool::Submit(std::shared_ptr<WalkJob> job) {
	{
		std::lock_guard lock(m_mutex);
		m_queue.push_back(std::move(job));
	}
	m_work.notify_one();
}

void WalkPool::Resize(uint32_t workers) {
	workers = std::clamp<uint32_t>(workers, 1u, kMaxWorkers);
	if (workers == m_threads.size()) return;
	while (m_threads.size() > workers) {
		m_threads.back().request_stop();
		m_work.notify_all();
		m_threads.pop_back(); // joins after it finishes its job
	}
	while (m_threads.size() < workers) {
		const auto index = static_cast<uint32_t>(m_threads.size());
		m_threads.emplace_back([this, index](const std::stop_token& stop) { Run(index, stop); });
	}
	m_workers.store(workers, std::memory_order_relaxed);
}

size_t WalkPool::Queued() const {
	std::lock_guard lock(m_mutex);
	return m_queue.size();
}

uint32_t WalkPool::Running() const {
	std::lock_guard lock(m_mutex);
	return m_running;
}

uint32_t WalkPool::Workers() const {
	return m_workers.load(std::memory_order_relaxed);
}

void WalkPool::Quiesce() {
	std::unique_lock lock(m_mutex);
	m_idle.wait(lock, [this] { return m_queue.empty() && m_running == 0; });
}

void WalkPool::Run(uint32_t index, const std::stop_token& stop) {
	char name[16];
	std::snprintf(name, sizeof(name), "KytyWalk%u", index);
#if defined(__APPLE__)
	pthread_setname_np(name);
#else
	pthread_setname_np(pthread_self(), name);
#endif
	for (;;) {
		std::shared_ptr<WalkJob> job;
		{
			std::unique_lock lock(m_mutex);
			if (!m_work.wait(lock, stop, [this] { return !m_queue.empty(); })) return;
			job = std::move(m_queue.front());
			m_queue.pop_front();
			m_running++;
		}
		// The recorder may have taken it already (TakeOrWait); then there is nothing to do.
		if (job->TryClaim()) {
			const auto* context = Context();
			if (context != nullptr) {
				job->Run(*context);
			} else {
				job->ok = false;
				job->Finish();
			}
		}
		{
			std::lock_guard lock(m_mutex);
			m_running--;
		}
		m_idle.notify_all();
	}
}

namespace {
std::atomic<bool> g_journal_active {false};
} // namespace

WalkReader::ViewPublisher& Publisher() {
	static WalkReader::ViewPublisher publisher;
	return publisher;
}

WalkReader::WriteJournal& Journal() {
	static WalkReader::WriteJournal journal;
	return journal;
}

void SetJournalActive() {
	g_journal_active.store(true, std::memory_order_release);
}

void NoteBackingWrite(uint64_t host_address, uint64_t size) {
	if (size != 0 && g_journal_active.load(std::memory_order_acquire)) {
		Journal().Note(host_address, host_address + size);
	}
}

WalkPool& Pool() {
	static WalkPool pool([] {
		const char* value = std::getenv("KYTY_GPU_FAN_WORKERS");
		const int   n     = value != nullptr ? std::atoi(value) : 2;
		return static_cast<uint32_t>(n > 0 ? n : 2);
	}());
	return pool;
}

void Count(Outcome outcome) {
	g_outcomes[static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
}
void CountSubmitted() {
	g_submitted.fetch_add(1, std::memory_order_relaxed);
}
void CountStolen() {
	g_stolen.fetch_add(1, std::memory_order_relaxed);
}
void CountShadowMismatch() {
	g_shadow_mismatch.fetch_add(1, std::memory_order_relaxed);
}
uint64_t TakeCount(Outcome outcome) {
	return g_outcomes[static_cast<size_t>(outcome)].exchange(0, std::memory_order_relaxed);
}
uint64_t TakeSubmitted() {
	return g_submitted.exchange(0, std::memory_order_relaxed);
}
uint64_t TakeStolen() {
	return g_stolen.exchange(0, std::memory_order_relaxed);
}
uint64_t TakeShadowMismatch() {
	return g_shadow_mismatch.exchange(0, std::memory_order_relaxed);
}

bool SameResult(const IR::ResourceSnapshot& a, const IR::ResourceSpecialization& as,
                const IR::ResourceSnapshot& b, const IR::ResourceSpecialization& bs) {
	return as == bs && a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
	       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data &&
	       a.specialization_reads == b.specialization_reads && a.uniform_fill == b.uniform_fill;
}

} // namespace Libs::Graphics::WalkFan
