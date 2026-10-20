#include "graphics/host_gpu/renderer/gpuPipeline.h"

#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"

#include <cstdlib>
#include <cstring>
#include <memory>

namespace Libs::Graphics::GpuPipeline {

namespace {

thread_local bool t_on_back = false;

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
	m_thread = std::jthread([this](const std::stop_token& stop) { Run(stop); });
}

RecordThread::~RecordThread() {
	Drain();
	m_thread.request_stop();
	m_work.notify_all();
}

void RecordThread::Enqueue(Common::UniqueFunction<void>&& job) {
	std::unique_lock lock(m_mutex);
	m_done.wait(lock, [this] { return m_jobs.size() < kMaxQueued; });
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

bool RecordThread::Idle() const {
	std::lock_guard lock(m_mutex);
	return m_jobs.empty() && !m_busy;
}

void RecordThread::Run(const std::stop_token& stop) {
	GuestGpu::MarkGpuHelperThread();
	t_on_back = true;
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
		job();
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

void DrainForFront() {
	if (t_on_back) {
		return;
	}
	auto& instance = Instance();
	if (instance && GuestGpu::IsGpuThread()) {
		instance->Drain();
	}
}

} // namespace Libs::Graphics::GpuPipeline
