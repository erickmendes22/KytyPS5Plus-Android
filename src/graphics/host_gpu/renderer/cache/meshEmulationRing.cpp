#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>

namespace Libs::Graphics {

namespace {

// Indirect: --gpu-indirect keeps its fixed-up draw records in a ring of this kind.
constexpr vk::BufferUsageFlags RingUsage = vk::BufferUsageFlagBits::eStorageBuffer |
                                           vk::BufferUsageFlagBits::eIndexBuffer |
                                           vk::BufferUsageFlagBits::eIndirectBuffer |
                                           vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::unique_ptr<Buffer> CreateRingBuffer(GraphicContext& graphics, CommandScheduler& scheduler,
                                         uint64_t size, const char* name) {
	auto buffer = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	                                       RingUsage, size);
	SetVulkanObjectNameF(graphics.device, buffer->Handle(), "{}", name);
	return buffer;
}

// Reservations of at least kLoggedReservationSize, and wraps or reuses of the current command
// buffer's ranges by ones of at least kLoggedWrapSize, are logged (first kLoggedReservations).
constexpr uint64_t kLoggedReservationSize = 16ull * 1024 * 1024;
constexpr uint64_t kLoggedWrapSize        = 1024ull * 1024;
constexpr uint32_t kLoggedReservations    = 4000;

} // namespace

// KYTY_MESH_RING_MIB=<n>: initial size of the mesh emulation ring (default DefaultInitialSize).
uint64_t MeshEmulationRing::ConfiguredInitialSize() {
	const char* value = std::getenv("KYTY_MESH_RING_MIB");
	if (value == nullptr || *value == '\0') {
		return DefaultInitialSize;
	}
	char*      end = nullptr;
	const auto mib = std::strtoull(value, &end, 10);
	if (end == nullptr || *end != '\0' || mib < 16 || mib > 2048) {
		LOGF("KYTY_MESH_RING_MIB=%s ignored (16..2048)\n", value);
		return DefaultInitialSize;
	}
	return mib << 20u;
}

// KYTY_MESH_RING_GROW_MIB=<n> (diagnostic): grow the ring to n MiB at the first reservation of
// at least kLoggedReservationSize, i.e. when the scene's large mesh draws appear.
uint64_t MeshEmulationRing::ConfiguredForcedGrowth() {
	const char* value = std::getenv("KYTY_MESH_RING_GROW_MIB");
	if (value == nullptr || *value == '\0') {
		return 0;
	}
	char*      end = nullptr;
	const auto mib = std::strtoull(value, &end, 10);
	if (end == nullptr || *end != '\0' || mib < 16 || mib > 2048) {
		LOGF("KYTY_MESH_RING_GROW_MIB=%s ignored (16..2048)\n", value);
		return 0;
	}
	return mib << 20u;
}

MeshEmulationRing::MeshEmulationRing(GraphicContext& graphics, CommandScheduler& scheduler,
                                     uint64_t size, const char* name)
    : m_graphics(graphics), m_scheduler(scheduler), m_name(name),
      m_buffer(CreateRingBuffer(graphics, scheduler, size, name)) {
	LOGF("%s: ring buffer %" PRIu64 " KiB at 0x%016" PRIx64 "\n", name, size >> 10u,
	     m_buffer->BufferDeviceAddress());
	m_buffer->LogAllocation(name);
}

MeshEmulationRing::~MeshEmulationRing() = default;

void MeshEmulationRing::DescribeFaultAddresses(std::span<const uint64_t> addresses) const {
	const auto describe = [&](const char* state, const Buffer& buffer, uint64_t tick) {
		const auto begin = buffer.BufferDeviceAddress();
		const auto end   = begin + buffer.Size();
		LOGF("[gpu-fault] %s %s [0x%016" PRIx64 ", 0x%016" PRIx64 ") tick=%" PRIu64 "\n", m_name,
		     state, begin, end, tick);
		for (const auto address: addresses) {
			if (address >= begin && address < end) {
				LOGF("[gpu-fault] 0x%016" PRIx64 " inside %s %s at +0x%" PRIx64 "\n", address, m_name,
				     state, address - begin);
			}
		}
	};
	describe("current", *m_buffer, 0);
	for (const auto& [buffer, tick]: m_retired) {
		describe("retired", *buffer, tick);
	}
	for (const auto& range: m_ranges) {
		LOGF("[gpu-fault] %s range [0x%" PRIx64 ", 0x%" PRIx64 ") tick=%" PRIu64 "\n", m_name,
		     range.begin, range.end, range.tick);
	}
}

// Pruning only consults GPU progress the renderer already observed. CommandScheduler::IsFree
// would query the timeline for every unfinished range on every emulated mesh draw, a kernel call
// that blocks for ~0.8 ms on Turnip (KGSL) while the GPU is busy.
bool MeshEmulationRing::KnownFree(uint64_t tick) const {
	return m_scheduler.GetMasterSemaphore().IsFree(tick);
}

void MeshEmulationRing::ReleaseRetired() {
	std::erase_if(m_retired, [this](const auto& retired) { return KnownFree(retired.second); });
}

void MeshEmulationRing::Grow(uint64_t size, const char* reason) {
	auto capacity = Capacity();
	while (capacity < size) {
		EXIT_IF(capacity > UINT64_MAX / 2u);
		capacity *= 2u;
	}
	LOGF("%s: growing ring buffer %" PRIu64 " -> %" PRIu64 " MiB (%" PRIu64 " bytes) %s\n",
	     m_name, Capacity() >> 20u, capacity >> 20u, size, reason);
	m_retired.emplace_back(std::move(m_buffer), m_scheduler.CurrentTick());
	m_buffer = CreateRingBuffer(m_graphics, m_scheduler, capacity, m_name);
	LOGF("%s: new buffer at 0x%016" PRIx64 " (old retired at tick %" PRIu64 ")\n", m_name,
	     m_buffer->BufferDeviceAddress(), m_retired.back().second);
	m_buffer->LogAllocation(m_name);
	m_offset = 0;
	m_ranges.clear();
}

MeshEmulationRing::Reservation MeshEmulationRing::Reserve(uint64_t size) {
	EXIT_IF(size == 0);
	ReleaseRetired();
	size = Common::AlignUp(size, Alignment);
	if (m_forced_growth != 0 && size >= kLoggedReservationSize) {
		if (m_forced_growth > Capacity()) {
			Grow(m_forced_growth, "forced by KYTY_MESH_RING_GROW_MIB");
		}
		m_forced_growth = 0;
	}
	if (size > Capacity()) {
		Grow(size, "for a larger draw");
	}
	const auto current = m_scheduler.CurrentTick();
	auto       begin   = Common::AlignUp(m_offset, Alignment);
	const bool wrapped = begin > Capacity() - size;
	if (wrapped) {
		begin = 0;
	}
	const auto end = begin + size;
	Reservation result {.buffer = m_buffer->Handle(), .offset = begin};
	// Ranges are in allocation order; any of them may overlap after a wrap.
	std::erase_if(m_ranges, [&](const Range& range) {
		if (range.end <= begin || range.begin >= end) {
			return KnownFree(range.tick);
		}
		if (range.tick == current) {
			result.reuses_current = true;
		} else if (!KnownFree(range.tick)) {
			const Common::PerfStats::ScopedWaitSite wait_site(Common::PerfStats::Wait::SemMeshRing);
			m_scheduler.Wait(range.tick);
		}
		return true;
	});
	if (!m_ranges.empty() && m_ranges.back().tick == current && m_ranges.back().end == begin) {
		m_ranges.back().end = end;
	} else {
		m_ranges.push_back({current, begin, end});
	}
	m_offset       = end;
	result.address = m_buffer->BufferDeviceAddress() + begin;
	if (size >= kLoggedReservationSize ||
	    (size >= kLoggedWrapSize && (wrapped || result.reuses_current))) {
		static std::atomic_uint32_t s_logged = 0;
		if (s_logged.fetch_add(1, std::memory_order_relaxed) < kLoggedReservations) {
			LOGF("%s: reserve 0x%" PRIx64 " at [0x%" PRIx64 ", 0x%" PRIx64 ") of 0x%" PRIx64
			     " addr=0x%016" PRIx64 " tick=%" PRIu64 "%s%s ranges=%zu\n",
			     m_name, size, begin, end, Capacity(), result.address, current,
			     wrapped ? " wrapped" : "", result.reuses_current ? " reuses_current" : "",
			     m_ranges.size());
		}
	}
	return result;
}

} // namespace Libs::Graphics
