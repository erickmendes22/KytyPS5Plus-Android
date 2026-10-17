#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"

#include <chrono>

namespace Libs::Graphics {

namespace {
// The tracker blocks in the driver for at most this long per wait so it can notice a stop.
constexpr uint64_t kTrackerWaitNs = 100'000'000;
} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	StopTracker();
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Advance(uint64_t counter) noexcept {
	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Refresh() {
	if (TrackerActive()) {
		return;
	}
	uint64_t   counter = 0;
	vk::Result result;
	{
		const Common::PerfStats::ScopedWait timing(Common::PerfStats::Wait::TimelineQuery);
		result = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Advance(counter);
}

void MasterSemaphore::MarkSubmitted(uint64_t tick) {
	{
		const std::lock_guard lock(m_submitted_mutex);
		auto                  known = m_submitted_tick.load(std::memory_order_acquire);
		while (known < tick && !m_submitted_tick.compare_exchange_weak(
		                           known, tick, std::memory_order_release,
		                           std::memory_order_relaxed)) {
		}
	}
	m_submitted_cv.notify_all();
}

void MasterSemaphore::WaitSubmitted(uint64_t tick) {
	if (SubmittedTick() >= tick) {
		return;
	}
	std::unique_lock lock(m_submitted_mutex);
	m_submitted_cv.wait(lock, [this, tick] { return SubmittedTick() >= tick; });
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	WaitSubmitted(tick);
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const Common::PerfStats::ScopedWait timing(Common::PerfStats::CurrentWaitSite());
	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Advance(tick);
	Refresh();
}

void MasterSemaphore::StartTracker() {
	if (m_tracker.joinable()) {
		return;
	}
	m_tracker_active.store(true, std::memory_order_release);
	m_tracker = std::jthread([this](const std::stop_token& stop) { TrackerThread(stop); });
}

void MasterSemaphore::StopTracker() {
	if (!m_tracker.joinable()) {
		return;
	}
	m_tracker.request_stop();
	m_submitted_cv.notify_all();
	m_tracker.join();
	m_tracker_active.store(false, std::memory_order_release);
}

void MasterSemaphore::TrackerThread(const std::stop_token& stop) {
	while (!stop.stop_requested()) {
		const auto next = KnownGpuTick() + 1;
		{
			std::unique_lock lock(m_submitted_mutex);
			m_submitted_cv.wait_for(lock, std::chrono::milliseconds(10), [this, &stop, next] {
				return stop.stop_requested() || SubmittedTick() >= next;
			});
			if (stop.stop_requested() || SubmittedTick() < next) {
				continue;
			}
		}
		vk::SemaphoreWaitInfo wait_info {};
		wait_info.semaphoreCount = 1;
		wait_info.pSemaphores    = &m_semaphore;
		wait_info.pValues        = &next;
		const auto result        = m_graphics.device.waitSemaphores(&wait_info, kTrackerWaitNs);
		if (result == vk::Result::eSuccess) {
			Advance(next);
		} else if (result != vk::Result::eTimeout) {
			EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
		}
	}
}

} // namespace Libs::Graphics
