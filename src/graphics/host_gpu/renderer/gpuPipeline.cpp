#include "graphics/host_gpu/renderer/gpuPipeline.h"
#include "graphics/guest_gpu/processWatchdog.h"

#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/perfWaits.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"
#include "graphics/host_gpu/renderer/rpStats.h"
#include "graphics/host_gpu/renderer/submitStats.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <pthread.h>
#include <cstring>
#include <memory>

namespace Libs::Graphics::GpuPipeline {

namespace {

thread_local bool t_on_back = false;

constexpr size_t kCauses = static_cast<size_t>(DrainCause::Count);
std::array<std::atomic<uint64_t>, kCauses> g_drains {};
std::array<std::atomic<uint64_t>, kCauses> g_drain_ns {};
std::array<std::atomic<uint64_t>, 512>     g_drain_ops {}; // opcode, or 256 + R of a custom NOP

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

constexpr const char* kCauseNames[kCauses] = {"sync", "packet", "suspend", "end", "inline_draw",
                                              "inline_dispatch", "other"};

void AppendReport(std::string& out, double per_frame) {
	char part[160];
	out += " pdrain:";
	for (size_t i = 0; i < kCauses; i++) {
		const auto n  = g_drains[i].exchange(0, std::memory_order_relaxed);
		const auto ns = g_drain_ns[i].exchange(0, std::memory_order_relaxed);
		if (n == 0) continue;
		std::snprintf(part, sizeof(part), " %s=%.1f(%.1fms)", kCauseNames[i],
		              static_cast<double>(n) * per_frame, static_cast<double>(ns) / 1e6 * per_frame);
		out += part;
	}
	std::array<std::pair<uint64_t, uint32_t>, 512> ops {};
	for (uint32_t op = 0; op < 512; op++) ops[op] = {g_drain_ops[op].exchange(0, std::memory_order_relaxed), op};
	std::partial_sort(ops.begin(), ops.begin() + 6, ops.end(), [](auto& a, auto& b) { return a.first > b.first; });
	out += " drain_ops:";
	for (size_t i = 0; i < 6 && ops[i].first != 0; i++) {
		if (ops[i].second >= 256u) {
			std::snprintf(part, sizeof(part), " nop.r%02x=%.1f", ops[i].second - 256u,
			              static_cast<double>(ops[i].first) * per_frame);
		} else {
			std::snprintf(part, sizeof(part), " 0x%02x=%.1f", ops[i].second,
			              static_cast<double>(ops[i].first) * per_frame);
		}
		out += part;
	}
	RpStats::Append(out, per_frame);
	SubmitStats::Append(out, per_frame);
	using WalkFan::Outcome;
	const auto take = [&](Outcome o) { return static_cast<double>(WalkFan::TakeCount(o)) * per_frame; };
	std::snprintf(part, sizeof(part), " fan: sub=%.0f ok=%.0f gpu_mod=%.1f no_backing=%.1f invalid=%.1f inval=%.1f",
	              static_cast<double>(WalkFan::TakeSubmitted()) * per_frame, take(Outcome::Accepted),
	              take(Outcome::RefusedGpuModified), take(Outcome::RefusedNoBacking),
	              take(Outcome::RefusedInvalid), take(Outcome::Invalidated));
	out += part;
	std::snprintf(part, sizeof(part), " failed=%.1f no_entry=%.1f call=%.1f memo=%.1f no_ctx=%.1f mismatch=%.1f rereq=%.1f",
	              take(Outcome::Failed), take(Outcome::NoEntry), take(Outcome::CallSource),
	              take(Outcome::Memo), take(Outcome::NoContext), take(Outcome::Mismatch),
	              take(Outcome::Rerequest));
	out += part;
	std::snprintf(part, sizeof(part), " reused=%.1f stolen=%.1f shadow_diff=%.1f workers=%u",
	              take(Outcome::Reused), static_cast<double>(WalkFan::TakeStolen()) * per_frame,
	              static_cast<double>(WalkFan::TakeShadowMismatch()) * per_frame, WalkFan::Pool().Workers());
	out += part;
}

std::unique_ptr<RecordThread>& Instance() {
	static std::unique_ptr<RecordThread> instance;
	return instance;
}

} // namespace

bool Enabled() {
	static const bool env_on = [] {
		const char* value = std::getenv("KYTY_GPU_PIPELINE");
		const bool  on    = value != nullptr && std::strcmp(value, "1") == 0;
		LOGF("KYTY_GPU_PIPELINE: two-stage GPU thread %s\n", on ? "on" : "off");
		return on;
	}();
	return LiveKnobs::Int("gpu_pipe", env_on ? 1 : 0) != 0;
}

RecordThread::RecordThread() {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, kStackBytes);
	if (pthread_create(&m_thread, &attr, Entry, this) != 0) {
		std::abort(); // no record thread: the two-stage GPU thread cannot run
	}
	pthread_attr_destroy(&attr);
}

RecordThread::~RecordThread() {
	Drain();
	m_stop.request_stop();
	m_work.notify_all();
	pthread_join(m_thread, nullptr);
}

void* RecordThread::Entry(void* self) {
	auto* thread = static_cast<RecordThread*>(self);
	thread->Run(thread->m_stop.get_token());
	return nullptr;
}

void RecordThread::Enqueue(Common::UniqueFunction<void>&& job) {
	std::unique_lock lock(m_mutex);
	if (m_jobs.size() >= kMaxQueued) {
		const Common::PerfStats::ScopedWait wait(Common::PerfStats::Wait::GpuPipeFull);
		m_done.wait(lock, [this] { return m_jobs.size() < kMaxQueued; });
	}
	m_jobs.push_back(std::move(job));
	m_work.notify_one();
}

void RecordThread::Drain() {
	std::unique_lock lock(m_mutex);
	if (m_jobs.empty() && !m_busy) {
		return;
	}
	const Common::PerfStats::ScopedWait wait(Common::PerfStats::Wait::GpuPipeDrain);
	m_done.wait(lock, [this] { return m_jobs.empty() && !m_busy; });
}

size_t RecordThread::Queued() const {
	std::lock_guard lock(m_mutex);
	return m_jobs.size();
}

bool RecordThread::Busy() const {
	std::lock_guard lock(m_mutex);
	return m_busy;
}

RecordThread* BackIfCreated() {
	return Instance().get();
}

bool RecordThread::Idle() const {
	std::lock_guard lock(m_mutex);
	return m_jobs.empty() && !m_busy;
}

void RecordThread::Run(const std::stop_token& stop) {
	GuestGpu::MarkGpuHelperThread();
	t_on_back = true;
	pthread_setname_np(pthread_self(), "KytyGpuRecord");
	Common::PerfStats::SampleThisThreadWallClock(); // --perf-stats wall stacks of both stages
	for (;;) {
		Common::UniqueFunction<void> job;
		{
			std::unique_lock lock(m_mutex);
			m_work.wait(lock, [&] { return stop.stop_requested() || !m_jobs.empty(); });
			if (m_jobs.empty()) {
				return;
			}
			job    = std::move(m_jobs.front());
			m_busy = true;
			m_jobs.pop_front();
		}
		m_done.notify_all(); // a slot is free
		{
			const Common::PerfStats::ScopedWait busy(Common::PerfStats::Wait::GpuPipeRecord);
			job();
		}
		{
			std::lock_guard lock(m_mutex);
			m_busy = false;
		}
		m_done.notify_all();
	}
}

RecordThread& Back() {
	auto& instance = Instance();
	if (!instance) {
		instance = std::make_unique<RecordThread>();
	}
	return *instance;
}

bool OnBack() {
	return t_on_back;
}

void DrainForFront(DrainCause cause, uint32_t opcode) {
	if (t_on_back) {
		return;
	}
	auto& instance = Instance();
	if (instance && GuestGpu::IsGpuThread() && !instance->Idle()) {
		const auto start = NowNs();
		ProcessWatchdog::SetPhase(ProcessWatchdog::Phase::Drain);
		instance->Drain();
		ProcessWatchdog::SetPhase(ProcessWatchdog::Phase::Process);
		const auto index = static_cast<size_t>(cause);
		g_drains[index].fetch_add(1, std::memory_order_relaxed);
		g_drain_ns[index].fetch_add(NowNs() - start, std::memory_order_relaxed);
		if (cause == DrainCause::Packet) g_drain_ops[opcode & 0x1ffu].fetch_add(1, std::memory_order_relaxed);
	}
}

void InstallReportSection() {
	static const bool installed = [] {
		Common::PerfStats::SetReportSection(AppendReport);
		return true;
	}();
	(void)installed;
}

} // namespace Libs::Graphics::GpuPipeline
