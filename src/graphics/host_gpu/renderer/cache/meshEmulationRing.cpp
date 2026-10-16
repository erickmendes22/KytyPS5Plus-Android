#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <cinttypes>

namespace Libs::Graphics {

namespace {

constexpr vk::BufferUsageFlags RingUsage = vk::BufferUsageFlagBits::eStorageBuffer |
                                           vk::BufferUsageFlagBits::eIndexBuffer |
                                           vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::unique_ptr<Buffer> CreateRingBuffer(GraphicContext& graphics, CommandScheduler& scheduler,
                                         uint64_t size) {
	auto buffer = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	                                       RingUsage, size);
	SetVulkanObjectNameF(graphics.device, buffer->Handle(), "Kyty.MeshEmulationRing");
	return buffer;
}

} // namespace

MeshEmulationRing::MeshEmulationRing(GraphicContext& graphics, CommandScheduler& scheduler,
                                     uint64_t size)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_buffer(CreateRingBuffer(graphics, scheduler, size)) {
	LOGF("Mesh emulation: ring buffer %" PRIu64 " MiB\n", size >> 20u);
}

MeshEmulationRing::~MeshEmulationRing() = default;

// Pruning only consults GPU progress the renderer already observed. CommandScheduler::IsFree
// would query the timeline for every unfinished range on every emulated mesh draw, a kernel call
// that blocks for ~0.8 ms on Turnip (KGSL) while the GPU is busy.
bool MeshEmulationRing::KnownFree(uint64_t tick) const {
	return m_scheduler.GetMasterSemaphore().IsFree(tick);
}

void MeshEmulationRing::ReleaseRetired() {
	std::erase_if(m_retired, [this](const auto& retired) { return KnownFree(retired.second); });
}

void MeshEmulationRing::Grow(uint64_t size) {
	auto capacity = Capacity();
	while (capacity < size) {
		EXIT_IF(capacity > UINT64_MAX / 2u);
		capacity *= 2u;
	}
	LOGF("Mesh emulation: growing ring buffer %" PRIu64 " -> %" PRIu64 " MiB for a %" PRIu64
	     " byte draw\n",
	     Capacity() >> 20u, capacity >> 20u, size);
	m_retired.emplace_back(std::move(m_buffer), m_scheduler.CurrentTick());
	m_buffer = CreateRingBuffer(m_graphics, m_scheduler, capacity);
	m_offset = 0;
	m_ranges.clear();
}

MeshEmulationRing::Reservation MeshEmulationRing::Reserve(uint64_t size) {
	EXIT_IF(size == 0);
	ReleaseRetired();
	size = Common::AlignUp(size, Alignment);
	if (size > Capacity()) {
		Grow(size);
	}
	auto begin = Common::AlignUp(m_offset, Alignment);
	if (begin > Capacity() - size) {
		begin = 0;
	}
	const auto end     = begin + size;
	const auto current = m_scheduler.CurrentTick();
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
	return result;
}

} // namespace Libs::Graphics
