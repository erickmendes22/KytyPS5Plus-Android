#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/gpuPipeline.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <cstdlib>
#include <bit>
#include <algorithm>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics, Config::SubmitThreadEnabled()),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	// KYTY_GPU_PIPELINE: a fault the front takes (its SRT walk read GPU-written memory) is handled
	// with the record thread drained, like every other renderer access of the front.
	GpuPipeline::DrainForFront();
	if (access == PageFaultAccess::Write) {
		const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemWriteFault);
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		// KYTY_FAULT_WINDOW_KB=<n> (power of two, 8..1024; 0 = off): a write fault also makes
		// the clean buffer-tracked pages of its aligned n KiB block writable, so a guest that
		// fills a buffer page by page takes one fault per block instead of one per 4 KiB page
		// (Bloodborne-Android unprotects 64 KiB). Pages the GPU wrote keep their read-back
		// fault; pages an image watches stay write-protected by the texture cache's watcher.
		// The cost is uploading the window's untouched pages on the next GPU use.
		// The live knob fault_kb=<n> overrides the start value while a game runs.
		static const int64_t start_kb = [] {
			const char* value = std::getenv("KYTY_FAULT_WINDOW_KB");
			return value != nullptr ? static_cast<int64_t>(std::strtoull(value, nullptr, 10)) : int64_t {0};
		}();
		const auto     kb     = static_cast<uint64_t>(std::max<int64_t>(LiveKnobs::Int("fault_kb", start_kb), 0));
		const uint64_t window = kb >= 8 && kb <= 1024 && std::has_single_bit(kb) ? kb * 1024ull : 0ull;
		if (window != 0) {
			const auto begin = fault_vaddr & ~(window - 1);
			if (IsMapped(begin, window)) {
				m_buffer_cache.OpenWriteWindow(begin, window);
			}
		}
	} else {
		const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemReadFault);
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_buffer_cache.QueueBdaSync(vaddr, size);
	m_buffer_cache.NoteMappedEnd(vaddr, size);
	LogMappedSpread(vaddr, size);
}

// How much of the dense BDA page table (one entry per 16 KiB page of the lower window plus the
// extended window) the guest's GPU mappings actually reach: logged when a high-water mark moves
// by a whole MiB of table (UE5 titles move it by 64 KiB a few hundred times while booting).
void RenderContext::LogMappedSpread(uint64_t vaddr, uint64_t size) {
	const auto end      = vaddr + size;
	const bool extended = vaddr >= LibKernel::Memory::kExtendedMemoryBase;
	bool       moved    = false;
	m_mapped_bytes += size;
	if (!extended && end > m_mapped_lower_high) {
		m_mapped_lower_high = end;
		moved               = true;
	}
	if (extended && (m_mapped_ext_low == 0 || vaddr < m_mapped_ext_low)) {
		m_mapped_ext_low = vaddr;
		moved            = true;
	}
	if (extended && end > m_mapped_ext_high) {
		m_mapped_ext_high = end;
		moved             = true;
	}
	const auto table_mib = [](uint64_t bytes) { return (bytes >> BufferCache::CACHING_PAGEBITS) * 8u >> 20u; };
	const auto lower_mib = table_mib(m_mapped_lower_high);
	const auto ext_mib   = table_mib(m_mapped_ext_high - m_mapped_ext_low);
	if (moved && (lower_mib != m_logged_lower_mib || ext_mib != m_logged_ext_mib || m_logged_lower_mib == UINT64_MAX)) {
		m_logged_lower_mib = lower_mib;
		m_logged_ext_mib   = ext_mib;
		LOGF("GPU mapped spread: lower high=0x%010" PRIx64 " (%" PRIu64 " MiB of table) extended=[0x%010" PRIx64
		     ", 0x%010" PRIx64 ") (%" PRIu64 " MiB of table) mapped %" PRIu64 " MiB in %zu ranges\n",
		     m_mapped_lower_high, lower_mib, m_mapped_ext_low, m_mapped_ext_high, ext_mib,
		     m_mapped_bytes >> 20u, [&] {
			     size_t count = 0;
			     m_mapped_ranges.ForEach([&](uint64_t, uint64_t) { ++count; });
			     return count;
		     }());
	}
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	// Grow the sized page table to the mapped high-water before this draw prepares its bindings.
	m_buffer_cache.GrowBdaTableToMapped();
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_buffer_cache.SynchronizeBda(m_mapped_ranges);
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
	m_buffer_cache.PollLoopGuardLog();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
