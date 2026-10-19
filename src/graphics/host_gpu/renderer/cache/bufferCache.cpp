#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/hostFaultAccess.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/gpuFatal.h"
#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/meshBoundsCheck.h"
#include "kernel/memory.h"

#include <algorithm>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write) {
	const auto [mapped, offset] = staging.Map(size, alignment);
	if (mapped != nullptr) {
		write(mapped, offset);
		staging.Commit();
		return {&staging, offset};
	}

	// Large transfers use the same one-time host allocation as ordinary buffer uploads.
	auto temporary = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                         usage, size);
	write(temporary->Mapped().data(), 0);
	temporary->Flush(0, size);
	auto* buffer = temporary.get();
	scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering(PassEnd::Buffer);
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		Common::PerfStats::Count(Common::PerfStats::Counter::UploadBytes, chunk);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
		m_memory_tracker.QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}

	int64_t  start_ns = 0;
	uint64_t writer   = 0;
	uint64_t lag      = 0;
	if constexpr (!async) {
		if (m_targeted_readback || Common::PerfStats::Enabled()) {
			start_ns = Common::PerfStats::Detail::MonotonicNs();
			for (const auto& copy: copies) {
				const auto begin = buffer_address + copy.srcOffset;
				writer           = std::max(writer, WriterTick(begin, begin + copy.size));
			}
			const auto current = m_scheduler.CurrentTick();
			lag = current - (writer != 0 ? writer : m_scheduler.GetMasterSemaphore().KnownGpuTick());
		}
		if (m_targeted_readback) {
			std::vector<std::pair<uint64_t, uint64_t>> ranges;
			ranges.reserve(copies.size());
			for (const auto& copy: copies) {
				ranges.emplace_back(copy.srcOffset, copy.size);
			}
			const auto mode = buffer.ReadbackView().empty()
			                      ? Common::PerfStats::ReadbackMode::Count
			                      : ReadBackInPlace(buffer, ranges, writer);
			if (mode != Common::PerfStats::ReadbackMode::Count) {
				Common::PerfStats::RecordReadback(
				    Common::PerfStats::CurrentWaitSite(), m_readback_reader, vaddr, total_size,
				    static_cast<uint64_t>(Common::PerfStats::Detail::MonotonicNs() - start_ns), lag,
				    mode);
				return true;
			}
		}
	}

	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering(PassEnd::Buffer);
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());
	Common::PerfStats::Count(Common::PerfStats::Counter::DownloadBytes, total_size);

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const Common::PerfStats::ScopedWaitSite wait_site(
		    Common::PerfStats::CurrentWaitSite() == Common::PerfStats::Wait::SemOther
		        ? Common::PerfStats::Wait::SemDownload
		        : Common::PerfStats::CurrentWaitSite());
		Common::PerfStats::Count(Common::PerfStats::Counter::SyncDownloads);
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
		if (start_ns != 0) {
			Common::PerfStats::RecordReadback(
			    Common::PerfStats::CurrentWaitSite(), m_readback_reader, vaddr, total_size,
			    static_cast<uint64_t>(Common::PerfStats::Detail::MonotonicNs() - start_ns), lag,
			    Common::PerfStats::ReadbackMode::CopyFlush);
		}
	}
	return true;
}

Common::PerfStats::ReadbackMode
BufferCache::ReadBackInPlace(Buffer& buffer, const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                             uint64_t writer) {
	using Common::PerfStats::ReadbackMode;
	const auto view = buffer.ReadbackView();
	EXIT_IF(view.empty() || ranges.empty());
	const Common::PerfStats::ScopedWaitSite wait_site(
	    Common::PerfStats::CurrentWaitSite() == Common::PerfStats::Wait::SemOther
	        ? Common::PerfStats::Wait::SemDownload
	        : Common::PerfStats::CurrentWaitSite());
	auto&        master = m_scheduler.GetMasterSemaphore();
	ReadbackMode mode   = ReadbackMode::InPlaceDone;
	if (writer != 0 && !master.IsFree(writer)) {
		const auto current = m_scheduler.CurrentTick();
		if (writer >= current) {
			// The open command buffer wrote these bytes: submit it (its end makes every write
			// visible to the host) and wait for it.
			mode = ReadbackMode::InPlaceFlush;
			m_scheduler.Wait(current);
		} else {
			// Only the writer's command buffer, not the backlog queued after it.
			mode = ReadbackMode::InPlaceWait;
			m_scheduler.Wait(writer);
		}
	}
	// Guest-memory completions queued up to the writer (e.g. an older asynchronous download of
	// the same bytes) must publish before these newer bytes do.
	m_scheduler.WaitPriorityOperations(writer != 0 ? writer : master.KnownGpuTick());

	uint64_t lowest = UINT64_MAX, highest = 0, bytes = 0;
	for (const auto& [offset, size]: ranges) {
		EXIT_IF(offset > view.size() || size > view.size() - offset);
		lowest  = std::min(lowest, offset);
		highest = std::max(highest, offset + size);
		bytes += size;
	}
	buffer.InvalidateReadback(lowest, highest - lowest);
	const auto buffer_address = buffer.CpuAddress();
	for (const auto& [offset, size]: ranges) {
		Libs::LibKernel::Memory::WriteBacking(buffer_address + offset, view.data() + offset, size);
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::DownloadBytes, bytes);
	if (mode != ReadbackMode::InPlaceDone) {
		Common::PerfStats::Count(Common::PerfStats::Counter::SyncDownloads);
	}
	return mode;
}

void BufferCache::MarkWritten(uint64_t vaddr, uint64_t size) {
	if (!m_targeted_readback && !Common::PerfStats::Enabled()) {
		return;
	}
	const auto end = vaddr + size;
	if (!m_pending_writes.empty() && m_pending_writes.back().first <= vaddr &&
	    m_pending_writes.back().second >= end) {
		return;
	}
	m_pending_writes.emplace_back(vaddr, end);
}

void BufferCache::StampPendingWritesSlow() {
	const auto tick = m_scheduler.CurrentTick();
	for (const auto& [begin, end]: m_pending_writes) {
		SetWriteTick(begin, end, tick);
	}
	m_pending_writes.clear();
	// Completed writers need no entry ("none" reads as complete); keep the map small.
	constexpr size_t PruneThreshold = 4096;
	if (m_write_ticks.size() > PruneThreshold) {
		const auto known = m_scheduler.GetMasterSemaphore().KnownGpuTick();
		std::erase_if(m_write_ticks, [known](const auto& entry) { return entry.second.tick <= known; });
	}
}

void BufferCache::SetWriteTick(uint64_t begin, uint64_t end, uint64_t tick) {
	if (const auto exact = m_write_ticks.find(begin);
	    exact != m_write_ticks.end() && exact->second.end == end) {
		exact->second.tick = tick;
		return;
	}
	auto first = m_write_ticks.lower_bound(begin);
	if (first != m_write_ticks.begin()) {
		const auto previous = std::prev(first);
		if (previous->second.end > begin) {
			first = previous;
		}
	}
	// Split partially covered neighbours; their outside parts keep their own ticks.
	std::array<std::pair<uint64_t, WriteTick>, 2> kept {};
	size_t                                        kept_count = 0;
	for (auto it = first; it != m_write_ticks.end() && it->first < end;) {
		const auto [entry_begin, entry] = *it;
		it                              = m_write_ticks.erase(it);
		if (entry_begin < begin) {
			kept[kept_count++] = {entry_begin, {begin, entry.tick}};
		}
		if (entry.end > end) {
			kept[kept_count++] = {end, {entry.end, entry.tick}};
		}
	}
	for (size_t i = 0; i < kept_count; i++) {
		m_write_ticks.emplace(kept[i].first, kept[i].second);
	}
	m_write_ticks.emplace(begin, WriteTick {end, tick});
}

uint64_t BufferCache::WriterTick(uint64_t begin, uint64_t end) const {
	for (const auto& [pending_begin, pending_end]: m_pending_writes) {
		if (pending_begin < end && begin < pending_end) {
			return m_scheduler.CurrentTick();
		}
	}
	uint64_t tick  = 0;
	auto     first = m_write_ticks.upper_bound(begin);
	if (first != m_write_ticks.begin()) {
		--first;
	}
	for (auto it = first; it != m_write_ticks.end() && it->first < end; ++it) {
		if (it->second.end > begin) {
			tick = std::max(tick, it->second.tick);
		}
	}
	return tick;
}

namespace {

// VK_EXT_device_fault: which known buffers lie at or near each reported fault address.
const BufferCache* g_fault_cache = nullptr;

void DumpFaultNeighbors(vk::Result /*result*/) {
	if (g_fault_cache != nullptr) {
		g_fault_cache->DescribeFaultAddresses(ReportedFaultAddresses());
		g_fault_cache->DumpMeshBoundsLog();
	}
}

} // namespace

void BufferCache::DescribeFaultAddresses(std::span<const uint64_t> addresses) const {
	if (addresses.empty()) {
		return;
	}
	constexpr uint64_t Near = 4 * MiB;
	const auto describe = [&](const char* name, const Buffer& buffer) {
		if (!buffer.HasDeviceAddress()) {
			return;
		}
		const auto begin = buffer.BufferDeviceAddress();
		const auto end   = begin + buffer.Size();
		for (const auto address: addresses) {
			if (address + Near >= begin && address < end + Near) {
				LOGF("[gpu-fault] 0x%016" PRIx64 " %s %s [0x%016" PRIx64 ", 0x%016" PRIx64
				     ") cpu=0x%010" PRIx64 "\n",
				     address, address >= begin && address < end ? "inside" : "near", name, begin,
				     end, buffer.CpuAddress());
			}
		}
	};
	describe("bda-pagetable", m_bda_pagetable_buffer);
	describe("staging", m_staging_buffer);
	describe("stream", m_stream_buffer);
	describe("download", m_download_buffer);
	describe("device-stream", m_device_buffer);
	if (m_mesh_emulation_ring) {
		m_mesh_emulation_ring->DescribeFaultAddresses(addresses);
	}
	if (m_indirect_draw_ring) {
		m_indirect_draw_ring->DescribeFaultAddresses(addresses);
	}
	for (const auto& [cpu_address, id]: m_buffers) {
		(void)cpu_address;
		describe("guest-buffer", m_slot_buffers[id]);
	}
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache), m_targeted_readback(Config::TargetedReadbackEnabled()) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	g_fault_cache = this;
	AddGpuFatalHook(DumpFaultNeighbors);
	if (MeshBoundsCheck::Enabled()) {
		// Mesh shaders bake the violation log's address in when they compile.
		GetMeshEmulationRing().EnableBoundsLog();
	}
	if (m_graphics.MemoryBudgetEnabled()) {
		m_budget_mode = true;
		ApplyBudgetThresholds(m_graphics.MemoryBudget());
		return;
	}
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

// --gpu-memory-budget: clean buffers unused for a while go from the texture cache's pressure
// threshold on; GPU-written ones (a download and a full GPU wait each) only past the budget itself.
void BufferCache::ApplyBudgetThresholds(uint64_t budget) {
	m_trigger_gc_memory  = GpuBudget::ForBudget(budget).pressure;
	m_critical_gc_memory = budget;
}

void BufferCache::DumpMeshBoundsLog() const {
	if (m_mesh_emulation_ring) {
		m_mesh_emulation_ring->DumpBoundsLog();
	}
}

uint64_t BufferCache::MeshRingCapacity() const noexcept {
	return m_mesh_emulation_ring ? m_mesh_emulation_ring->Capacity() : 0;
}

MeshEmulationRing& BufferCache::GetMeshEmulationRing() {
	if (!m_mesh_emulation_ring) {
		m_mesh_emulation_ring = std::make_unique<MeshEmulationRing>(
		    m_graphics, m_scheduler, MeshEmulationRing::ConfiguredInitialSize());
	}
	return *m_mesh_emulation_ring;
}

MeshEmulationRing& BufferCache::GetIndirectDrawRing() {
	if (!m_indirect_draw_ring) {
		constexpr uint64_t IndirectRingSize = 1024ull * 1024;
		m_indirect_draw_ring = std::make_unique<MeshEmulationRing>(
		    m_graphics, m_scheduler, IndirectRingSize, "Kyty.IndirectDrawRing");
	}
	return *m_indirect_draw_ring;
}

BufferCache::~BufferCache() {
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

namespace {

// KYTY_READBACK_DIFF=1 (diagnostics): for read-fault downloads (the first 4 and then every 32nd
// per reader and 4 KiB page, 3000 lines at most), log whether the faulting bytes were inside a
// GPU-written range and whether the download changed them ("[perf-trace] rbdiff ..."), i.e.
// whether the CPU reader waited for bytes the GPU never wrote.
bool ReadbackDiffEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_READBACK_DIFF");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

} // namespace

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	uint32_t reader = Common::PerfStats::kReaderOther;
	if (!GuestGpu::IsGpuThread()) {
		reader = is_write ? Common::PerfStats::kReaderGuestWrite : Common::PerfStats::kReaderGuestRead;
	} else if (const auto opcode = Common::PerfStats::CurrentPm4Opcode(); opcode != 0) {
		reader = opcode;
	}
	m_scheduler.Context().GetGpu().SendCommandSync(
	    [this, vaddr, size, is_write, reader, site = Common::PerfStats::CurrentWaitSite()] {
		const Common::PerfStats::ScopedWaitSite wait_site(site);
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		m_readback_reader = reader;
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

		static uint32_t diff_logged = 0;
		static std::unordered_map<uint64_t, uint32_t> diff_seen;
		bool diff = false;
		if (!is_write && ReadbackDiffEnabled() && diff_logged < 3000u) {
			const auto n = diff_seen[(static_cast<uint64_t>(reader) << 48u) ^ (vaddr >> 12u)]++;
			diff = n < 4u || n % 32u == 0u;
		}
		const uint64_t diff_base = vaddr & ~uint64_t {63};
		std::array<uint8_t, 64> diff_before {};
		uint64_t diff_range_begin = 0, diff_range_end = 0, diff_page_gpu_bytes = 0;
		if (diff) {
			(void)LibKernel::Memory::TryReadSparseBacking(diff_base, diff_before.data(), diff_before.size());
			m_gpu_modified_ranges.ForEach([&](uint64_t begin, uint64_t end) {
				if (begin <= vaddr && vaddr < end) {
					diff_range_begin = begin;
					diff_range_end   = end;
				}
			});
			const uint64_t page = vaddr & ~uint64_t {4095};
			m_gpu_modified_ranges.ForEachInRange(page, 4096, [&](uint64_t begin, uint64_t end) {
				diff_page_gpu_bytes += end - begin;
			});
		}

		const bool downloaded = DownloadBufferMemory<false>(buffer, vaddr, size);
		if (diff) {
			std::array<uint8_t, 64> after {};
			(void)LibKernel::Memory::TryReadSparseBacking(diff_base, after.data(), after.size());
			const auto at16 = (vaddr - diff_base) & ~uint64_t {15};
			const bool changed16 = std::memcmp(diff_before.data() + at16, after.data() + at16, 16) != 0;
			const bool changed64 = diff_before != after;
			// The faulting host code of a read fault the GPU thread took itself (a pm4 reader): pc
			// and lr as module offsets. Registers only, never dereferenced (a stale frame pointer
			// would fault in here).
			std::string where;
#if !defined(_WIN32)
			if (GuestGpu::IsGpuThread() && reader != Common::PerfStats::kReaderGuestRead &&
			    reader != Common::PerfStats::kReaderGuestWrite) {
				unsigned long long pc = 0, lr = 0, fp = 0;
				Common::HostException::LastFaultRegisters(&pc, &lr, &fp);
				for (const auto frame: {pc, lr}) {
					Dl_info info {};
					if (frame != 0 && dladdr(reinterpret_cast<const void*>(frame), &info) != 0 &&
					    info.dli_fbase != nullptr) {
						where += fmt::format(" +0x{:x}", frame - reinterpret_cast<uint64_t>(info.dli_fbase));
					} else {
						where += fmt::format(" 0x{:x}", frame);
					}
				}
			}
#endif
			Common::PerfStats::EmitLine(fmt::format(
			    "[perf-trace] rbdiff reader=0x{:x} addr=0x{:x} downloaded={} in_range={} range=0x{:x}+0x{:x} "
			    "page_gpu_bytes={} changed16={} changed64={} at{}",
			    reader, vaddr, downloaded ? 1 : 0, diff_range_end != 0 ? 1 : 0, diff_range_begin,
			    diff_range_end - diff_range_begin, diff_page_gpu_bytes, changed16 ? 1 : 0, changed64 ? 1 : 0,
			    where));
			diff_logged++;
		}
		if (downloaded) {
			// A write releases the same range below, in the same critical section as its CPU
			// claim: releasing it here as well would be a second lock acquisition on it, which is
			// the window this change closes.
			if (!is_write) {
				m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
			}
		}
		if (is_write) {
			// The guest writes these bytes, so whatever the GPU wrote is superseded -- and the
			// GPU's release and the CPU's claim have to happen under one lock. As two calls, an
			// upload on the renderer thread can re-mark a page GPU-dirty in between and
			// RegionManager then exits on "CPU dirty state conflicts with GPU dirty state". This
			// is also the path a write takes when there is nothing to download: the old code
			// marked the page CPU-dirty over the GPU's untouched claim.
			m_memory_tracker.TakeGpuPagesForCpu(vaddr, size);
		}
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	// The merged bytes reach the new buffer on the GPU timeline, in the open command buffer.
	MarkWritten(overlap.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering(PassEnd::Buffer);
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		Common::PerfStats::Count(Common::PerfStats::Counter::UploadBytes, total_size);
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	const auto source = UploadMemory(m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset, copy.size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			copy.srcOffset += base_offset;
		}
	});
	return source.first->Handle();
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
		m_gpu_dirty_generation++;
		MarkWritten(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	return UploadMemory(m_graphics, m_scheduler, m_staging_buffer, size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
	    [&](uint8_t* mapped, uint64_t) {
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size)) {
			EXIT("BufferCache: failed to read mapped guest image backing\n");
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		// Raw guest writes bypass page-fault tracking when the pages are already
		// CPU-owned, so mark the range explicitly. Otherwise a later ObtainBuffer
		// can miss IsRegionCpuModified and serve a stale slot-buffer copy (e.g. a
		// 0xFFFFFFFF slot-queue reset never reaching its consumer, #1072).
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		m_memory_tracker.MarkRegionAsCpuModified(dst_vaddr, size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_budget_mode) {
		// The texture cache updated the budget for this tick just before.
		ApplyBudgetThresholds(m_graphics.MemoryBudget());
	}
	// Driver usage includes other caches and allocator reservations. Keep it
	// separate from the owned-byte counter decremented during unregistration.
	const auto used_memory = m_budget_mode ? m_graphics.GetLiveDeviceMemory()
	                         : m_graphics.CanReportMemoryUsage()
	                             ? m_graphics.GetDeviceMemoryUsage()
	                             : m_total_used_memory;
	if (used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count  = 0;
	uint64_t              retired_bytes = 0;
	uint64_t              largest_addr  = 0;
	uint64_t              largest_size  = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		retired_bytes += buffer.Size();
		if (buffer.Size() > largest_size) {
			largest_size = buffer.Size();
			largest_addr = buffer.CpuAddress();
		}
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (retire_count != 0) {
		// Device-loss diagnostics: which guest buffers the collector retired, and when.
		static std::atomic_uint32_t s_logged = 0;
		if (s_logged.fetch_add(1, std::memory_order_relaxed) < 3000) {
			LOGF("BufferCache GC: tick=%" PRIu64 " used=%" PRIu64 " MiB critical=%" PRIu64
			     " MiB aggressive=%d retired=%zu (%" PRIu64 " KiB, %zu GPU-written) largest guest=0x%"
			     PRIx64 " size=0x%" PRIx64 "\n",
			     tick, used_memory >> 20u, m_critical_gc_memory >> 20u, aggressive ? 1 : 0,
			     retire_count, retired_bytes >> 10u, dirty_buffers.size(), largest_addr,
			     largest_size);
		}
	}
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const Common::PerfStats::ScopedWaitSite wait_site(Common::PerfStats::Wait::SemGc);
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBda(const RangeSet& mapped) {
	// Take before walking: writes queued during synchronization remain pending.
	const auto pending = m_memory_tracker.TakeBdaSync();
	pending.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin, [this](uint64_t start, uint64_t finish) {
			SynchronizeBuffersInRange(start, finish - start);
		});
	});
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
