#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/perfWaits.h"
#include "common/slotVector.h"
#include "kernel/addressFold.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "kernel/addressLayout.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/meshEmulationRing.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	// --fault-window: after a CPU write fault, also make the clean tracked pages of the
	// surrounding window writable (see MemoryTracker::MarkCleanPagesCpuModified).
	uint32_t               OpenWriteWindow(uint64_t vaddr, uint64_t size) {
		return m_memory_tracker.MarkCleanPagesCpuModified(Kernel::AddressFold::Fold(vaddr), size);
	}
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	// Mesh emulation output ring, created on the first emulated mesh draw.
	[[nodiscard]] MeshEmulationRing& GetMeshEmulationRing();
	// Device-loss diagnostics: logs the buffers containing or near each address.
	void DescribeFaultAddresses(std::span<const uint64_t> addresses) const;
	// KYTY_MESH_BOUNDS: logs the emulated mesh stage's recorded violations (GPU fatal hook).
	void DumpMeshBoundsLog() const;
	// --gpu-indirect fixed-up draw records, created on the first GPU indirect draw.
	[[nodiscard]] MeshEmulationRing& GetIndirectDrawRing();
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	// Bumped whenever bytes become GPU-written (GPU thread): a page seen without GPU-written
	// bytes stays so while this is unchanged.
	[[nodiscard]] uint64_t GpuDirtyGeneration() const { return m_gpu_dirty_generation; }
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               ProcessFaultBuffer();
	void               SynchronizeBda(const RangeSet& mapped);
	void               QueueBdaSync(uint64_t vaddr, uint64_t size) {
		m_memory_tracker.QueueBdaSync(Kernel::AddressFold::Fold(vaddr), size);
	}
	void               RunGarbageCollector();
	// Bytes of guest-mirroring buffers the cache owns, and the mesh emulation ring (0 if unused).
	[[nodiscard]] uint64_t TotalUsedMemory() const noexcept { return m_total_used_memory; }
	[[nodiscard]] uint64_t MeshRingCapacity() const noexcept;
	// --targeted-readback: GPU writes acquired since the last call are recorded by now; give them
	// the open command buffer's tick. The command processor calls this between PM4 packets.
	void StampPendingWrites() {
		if (!m_pending_writes.empty()) {
			StampPendingWritesSlow();
		}
	}

private:
	void ApplyBudgetThresholds(uint64_t budget);
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS,
	                                      Common::AddressLayout::kTrackerAddressBits, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	void SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Writer tracking for --targeted-readback (and --perf-stats readback lag): every GPU write to
	// guest-backed buffer bytes is pending until the next packet boundary, then stamped with the
	// tick of the command buffer that recorded it. Ticks the GPU has completed may be pruned, so
	// "no entry" means "complete".
	void MarkWritten(uint64_t vaddr, uint64_t size);
	void StampPendingWritesSlow();
	void SetWriteTick(uint64_t begin, uint64_t end, uint64_t tick);
	// Newest tick that wrote any byte of [begin, end): 0 when complete or unknown-but-complete,
	// CurrentTick() when a pending write overlaps (it is in the open command buffer).
	[[nodiscard]] uint64_t WriterTick(uint64_t begin, uint64_t end) const;
	// In-place readback of `ranges` (buffer offsets, sizes) from a host-visible buffer after
	// waiting only for their writer; returns how it waited.
	[[nodiscard]] Common::PerfStats::ReadbackMode
	ReadBackInPlace(Buffer& buffer, const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
	                uint64_t writer);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	uint64_t                                          m_gpu_dirty_generation = 0;
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	std::unique_ptr<MeshEmulationRing>                m_mesh_emulation_ring;
	std::unique_ptr<MeshEmulationRing>                m_indirect_draw_ring;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	bool     m_budget_mode        = false;
	struct WriteTick {
		uint64_t end  = 0;
		uint64_t tick = 0;
	};
	bool                                       m_targeted_readback = false;
	bool                                       m_track_writers     = false;
	std::map<uint64_t, WriteTick>              m_write_ticks;
	std::vector<std::pair<uint64_t, uint64_t>> m_pending_writes;
	uint32_t                                   m_readback_reader = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
