#include "graphics/host_gpu/renderer/pipeline/shaderWorkerPool.h"

#include <algorithm>
#include <cstdio>

#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace Libs::Graphics {

namespace {

thread_local bool t_on_worker_thread = false;

void NameCurrentThread(uint32_t index) {
	char name[16];
	std::snprintf(name, sizeof(name), "KytyShader%u", index);
#if defined(__APPLE__)
	pthread_setname_np(name);
#elif defined(__linux__) || defined(__ANDROID__)
	pthread_setname_np(pthread_self(), name);
#else
	(void)name;
#endif
}

} // namespace

void ShaderWorkerPool::Job::Execute() {
	Run();
	{
		// The lock orders the flag before a waiter's wake-up check (no lost notification).
		std::lock_guard lock(m_mutex);
		m_done.store(true, std::memory_order_release);
	}
	m_finished.notify_all();
}

void ShaderWorkerPool::Job::Wait() {
	std::unique_lock lock(m_mutex);
	m_finished.wait(lock, [this] { return m_done.load(std::memory_order_acquire); });
}

ShaderWorkerPool::ShaderWorkerPool(uint32_t threads): m_thread_count(threads) {
	m_threads.reserve(threads);
	for (uint32_t index = 0; index < threads; index++) {
		m_threads.emplace_back([this, index] { WorkerMain(index); });
	}
}

ShaderWorkerPool::~ShaderWorkerPool() {
	{
		std::lock_guard lock(m_mutex);
		m_stop = true;
		// Queued jobs are dropped unstarted: their owners' destructors release the inputs.
		m_queue.clear();
		m_background.clear();
	}
	m_wake.notify_all();
	for (auto& thread: m_threads) {
		thread.join();
	}
}

void ShaderWorkerPool::Submit(std::shared_ptr<Job> job) {
	if (m_threads.empty()) {
		if (!job->m_started.exchange(true, std::memory_order_acq_rel)) {
			job->Execute();
		}
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		m_queue.push_back(std::move(job));
	}
	m_wake.notify_one();
}

void ShaderWorkerPool::SubmitBackground(std::vector<std::shared_ptr<Job>> jobs) {
	if (m_threads.empty() || jobs.empty()) {
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		for (auto& job: jobs) {
			job->m_background.store(true, std::memory_order_relaxed);
			m_background.push_back(std::move(job));
		}
	}
	// Some workers do not take background jobs: wake all so an eligible one sees them.
	m_wake.notify_all();
}

void ShaderWorkerPool::Prioritize(const std::shared_ptr<Job>& job) {
	if (!job->m_background.load(std::memory_order_relaxed)) {
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		job->m_background.store(false, std::memory_order_relaxed);
		const auto found = std::ranges::find(m_background, job);
		if (found == m_background.end()) {
			return; // a worker took it already
		}
		m_background.erase(found);
		m_queue.push_back(job);
	}
	m_wake.notify_all();
}

void ShaderWorkerPool::RunNow(const std::shared_ptr<Job>& job) {
	if (job->Done()) {
		return;
	}
	bool run_here = false;
	{
		std::lock_guard lock(m_mutex);
		if (const auto found = std::ranges::find(m_queue, job); found != m_queue.end()) {
			m_queue.erase(found);
		} else if (job->m_background.load(std::memory_order_relaxed)) {
			if (const auto queued = std::ranges::find(m_background, job); queued != m_background.end()) {
				m_background.erase(queued);
			}
		}
		job->m_background.store(false, std::memory_order_relaxed);
		// Still false here means no worker took it (it was queued or never submitted).
		run_here = !job->m_started.exchange(true, std::memory_order_acq_rel);
	}
	if (run_here) {
		job->Execute();
	} else {
		// A worker took it already: it finishes soon.
		job->Wait();
	}
}

uint32_t ShaderWorkerPool::Pending() const {
	std::lock_guard lock(m_mutex);
	return static_cast<uint32_t>(m_queue.size());
}

bool ShaderWorkerPool::OnWorkerThread() {
	return t_on_worker_thread;
}

uint32_t ShaderWorkerPool::DefaultThreadCount(uint32_t hardware_threads) {
	// Leave most cores to the guest: the GPU thread plus the game's own threads keep them busy.
	if (hardware_threads <= 2) {
		return 1;
	}
	return std::clamp(hardware_threads / 2u - 1u, 1u, 3u);
}

void ShaderWorkerPool::WorkerMain(uint32_t index) {
	t_on_worker_thread = true;
	NameCurrentThread(index);
	for (;;) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock lock(m_mutex);
			const bool takes_background = index != 0 || m_thread_count == 1;
			m_wake.wait(lock, [&] {
				return m_stop || !m_queue.empty() || (takes_background && !m_background.empty());
			});
			if (m_stop) {
				return;
			}
			auto& queue = !m_queue.empty() ? m_queue : m_background;
			job         = std::move(queue.front());
			queue.pop_front();
			job->m_background.store(false, std::memory_order_relaxed);
			job->m_started.store(true, std::memory_order_release);
		}
		job->Execute();
	}
}

} // namespace Libs::Graphics
