#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <mutex>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

// Spins briefly, then sleeps in the kernel (futex) until the holder releases it: the texture
// cache's lock can be held across long GPU-thread work while many guest threads take
// write-tracking faults, and pure spinning burned several cores there.
class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		for (uint32_t spins = 0; m_lock.test_and_set(std::memory_order_acquire); spins++) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
			if (spins < SpinsBeforeSleep) {
#if defined(__x86_64__) || defined(_M_X64)
				_mm_pause();
#elif defined(_M_ARM64)
				__yield();
#elif defined(__aarch64__)
				asm volatile("yield");
#endif
				continue;
			}
			// seq_cst pairs with unlock(): either it sees this waiter or the wait sees the clear.
			m_waiters.fetch_add(1, std::memory_order_seq_cst);
			m_lock.wait(true, std::memory_order_seq_cst);
			m_waiters.fetch_sub(1, std::memory_order_relaxed);
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_seq_cst);
		if (m_waiters.load(std::memory_order_seq_cst) != 0) {
			m_lock.notify_one();
		}
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		static thread_local const uint32_t tid =
		    static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
		return tid;
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	static constexpr uint32_t SpinsBeforeSleep = 128;

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
	std::atomic_uint32_t m_waiters {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().FirstRangeFrom(start).first < end;
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateProtection<!enable, false>();
		} else {
			UpdateProtection<enable, true>();
		}
	}

	// Write-fault window: makes the pages of [vaddr, vaddr + size) that are tracked (neither CPU-
	// nor GPU-modified) CPU-modified, i.e. writable until the next upload, as one protection
	// change. GPU-modified pages keep their read-back-on-fault path. Returns the pages changed.
	uint32_t MarkCleanPagesCpuModified(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		uint32_t   changed      = 0;
		for (size_t page = start; page < end; page++) {
			if (!m_cpu_dirty.Get(page) && !m_gpu_dirty.Get(page)) {
				m_cpu_dirty.Set(page);
				changed++;
			}
		}
		if (changed != 0) {
			UpdateProtection<false, false>();
		}
		return changed;
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			bits.UnsetRange(start, end);
			if constexpr (source == DirtySource::Cpu) {
				UpdateProtection<true, false>();
			} else {
				UpdateProtection<false, true>();
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	TrackingSpinLock lock;

private:
	template <bool track, bool is_read>
	void UpdateProtection() {
		const auto protection = is_read ? ~m_gpu_dirty : m_cpu_dirty;
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
