#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/submitStats.h"
#include <chrono>

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/gpuFatal.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master,
                                           vk::CommandBufferLevel level, size_t grow_step)
    : m_graphics(graphics), m_master(master), m_level(level), m_grow_step(grow_step) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + m_grow_step);
	m_buffers.resize(first + m_grow_step);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = m_level;
	allocate.commandBufferCount = static_cast<uint32_t>(m_grow_step);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics,
                                   bool threaded_submit)
    : m_master(graphics), m_context(context), m_graphics(graphics), m_gpu_timer(graphics, m_master),
      m_command_pool(graphics, m_master),
      m_pass_pool(graphics, m_master, vk::CommandBufferLevel::eSecondary, 64), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	if (threaded_submit) {
		m_submit_thread = std::jthread([this](const std::stop_token& stop) { SubmitThread(stop); });
	}
	m_host_visible_writes = Config::TargetedReadbackEnabled();
	if (Config::TimelineTrackerEnabled()) {
		m_master.StartTracker();
	}
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	StopSubmitThread();
	m_master.StopTracker();
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering(PassEnd reason) {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering(reason);
	}
}

bool CommandScheduler::CanHoistBarrier(uint64_t image_pass_serial) const noexcept {
	return Active() && !m_command.IsInvalid() && m_command.IsRendering() &&
	       m_command.HoistsBarriers() && m_command.PassSerial() != image_pass_serial;
}

void CommandScheduler::Flush() {
	SubmitStats::NoteCaller(__builtin_return_address(0));
	SubmitInfo submit;
	Submit(submit);
	BeginNext();
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	SubmitStats::NoteCaller(__builtin_return_address(0));
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	SubmitStats::NoteCaller(__builtin_return_address(0));
	const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemFlush);
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemFinish);
	CheckActive();
	if (!m_command.IsInvalid()) {
		SubmitStats::NoteCaller(__builtin_return_address(0));
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		const Common::PerfStats::ScopedWaitSite site(
		    Common::PerfStats::CurrentWaitSite() == Common::PerfStats::Wait::SemOther
		        ? Common::PerfStats::Wait::SemFlush
		        : Common::PerfStats::CurrentWaitSite());
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	// Runs per draw and dispatch: query the timeline only when the oldest operation belongs to a
	// submitted command buffer not yet known to be complete. The query is a kernel call on some
	// drivers (Turnip on KGSL walks its emulated timeline).
	{
		std::lock_guard lock(m_operation_mutex);
		if (m_pending_operations.empty()) {
			return;
		}
		const auto tick = m_pending_operations.front().tick;
		if (tick >= CurrentTick()) {
			return;
		}
		if (!m_master.IsFree(tick)) {
			m_master.Refresh();
		}
	}
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		queue.push({std::move(operation), CurrentTick()});
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		{
			const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemPriority);
			m_master.Wait(operation.tick);
		}
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	const Common::PerfStats::ScopedWait timing(Common::PerfStats::Wait::GpuPriority);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	EXIT_IF(m_command.IsInvalid());
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	m_gpu_timer.Begin(m_command.m_buffer);
	m_dynamic_state.Invalidate();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	m_command.EndRendering(PassEnd::Submit);
	if (m_host_visible_writes) {
		// --targeted-readback reads GPU results straight from host-visible buffer memory once
		// the writer's tick completes: make this command buffer's writes available to the host.
		vk::MemoryBarrier2 barrier {};
		barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
		barrier.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
		barrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
		vk::DependencyInfo dependency {};
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers    = &barrier;
		m_command.m_buffer.pipelineBarrier2(dependency);
	}
	m_gpu_timer.End(m_command.m_buffer);
	m_command.End();
	EXIT_IF(m_graphics.queue == nullptr);

	// Ticks are allocated here, in recording order; the submit thread hands them to the driver
	// in the same order.
	const bool    external_semaphores = submit.num_wait_semaphores != 0 || submit.num_signal_semaphores != 0;
	PendingSubmit pending;
	pending.tick = m_master.NextTick();
	submit.AddSignal(m_master.Handle(), pending.tick);
	pending.info            = submit;
	pending.buffer          = m_command.m_buffer;
	pending.debug_op        = m_command.m_debug_op;
	pending.debug_submit_id = m_command.m_debug_submit_id;
	pending.debug_arg0      = m_command.m_debug_arg0;
	pending.debug_arg1      = m_command.m_debug_arg1;
	pending.debug_arg2      = m_command.m_debug_arg2;
	pending.debug_arg3      = m_command.m_debug_arg3;
	pending.debug_arg4      = m_command.m_debug_arg4;
	pending.work            = m_work_since_submit;

	if (m_submit_thread.joinable() && !external_semaphores) {
		{
			const std::lock_guard lock(m_submit_mutex);
			m_submit_queue.push(pending);
		}
		m_submit_available.notify_one();
	} else {
		// Binary semaphores must be signalled in submission order with their waits (present).
		DrainSubmitThread();
		QueueSubmit(pending);
	}
	m_gpu_timer.Submitted(pending.tick);

	m_command.m_buffer  = nullptr;
	m_work_since_submit = 0;
	return pending.tick;
}

void CommandScheduler::QueueSubmit(const PendingSubmit& pending) {
	const auto& submit = pending.info;
	const auto  buffer = pending.buffer;
	auto&       graphics = m_graphics;

	vk::TimelineSemaphoreSubmitInfo timeline_info {};
	timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
	timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
	timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
	timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

	vk::SubmitInfo submit_info {};
	submit_info.pNext                = &timeline_info;
	submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
	submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
	submit_info.pWaitDstStageMask    = submit.wait_stages.data();
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
	submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

	vk::Result result;
	{
		Common::LockGuard                   lock(graphics.queue_mutex);
		const Common::PerfStats::ScopedWait timing(Common::PerfStats::Wait::QueueSubmit);
		const auto submit_start = std::chrono::steady_clock::now();
		result = graphics.queue.submit(1, &submit_info, nullptr);
		SubmitStats::NoteSubmit(pending.work, static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count()));
		Common::PerfStats::Count(Common::PerfStats::Counter::Submits);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, pending.tick, pending.debug_op,
		                  pending.debug_submit_id, pending.debug_arg0, pending.debug_arg1,
		                  pending.debug_arg2, pending.debug_arg3, pending.debug_arg4);
	}
	if (GpuFatalTestDue()) {
		result = vk::Result::eErrorDeviceLost;
	}
	CheckGpuResult("vkQueueSubmit", result);
	m_master.MarkSubmitted(pending.tick);
}

void CommandScheduler::SubmitThread(const std::stop_token& stop) {
	KYTY_PROFILER_THREAD("Thread_Submit");
	for (;;) {
		PendingSubmit pending;
		{
			std::unique_lock lock(m_submit_mutex);
			m_submit_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_submit_queue.empty();
			});
			if (m_submit_queue.empty()) {
				// Stop only once everything queued has been submitted.
				return;
			}
			pending = m_submit_queue.front();
			m_submit_queue.pop();
			m_submit_active = true;
		}
		QueueSubmit(pending);
		{
			const std::lock_guard lock(m_submit_mutex);
			m_submit_active = false;
		}
		m_submit_drained.notify_all();
	}
}

void CommandScheduler::DrainSubmitThread() {
	if (!m_submit_thread.joinable()) {
		return;
	}
	std::unique_lock lock(m_submit_mutex);
	m_submit_drained.wait(lock, [this] { return m_submit_queue.empty() && !m_submit_active; });
}

void CommandScheduler::StopSubmitThread() {
	if (!m_submit_thread.joinable()) {
		return;
	}
	DrainSubmitThread();
	m_submit_thread.request_stop();
	m_submit_available.notify_all();
	m_submit_thread.join();
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
