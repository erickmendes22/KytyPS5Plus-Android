// --gpu-memory-budget: the texture cache collector when it keeps to one device-memory budget
// (gpuMemoryBudget.h) instead of the legacy heap-size thresholds.

#include "common/perfWaits.h"
#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <vector>

namespace Libs::Graphics {

namespace {

// Ticks (frame boundaries) since an image's last use before it may go. Nothing used by the frame
// just recorded qualifies, and destruction is deferred until the GPU has finished the current
// tick, so frames in flight never lose an image.
constexpr uint64_t kAgeNormal    = 16;
constexpr uint64_t kAgePressured = 12;
constexpr uint64_t kAgeCritical  = 6;

} // namespace

void TextureCache::ApplyBudgetThresholds(uint64_t budget) {
	const auto thresholds = GpuBudget::ForBudget(budget);
	m_trigger_gc_memory   = thresholds.trigger;
	m_pressure_gc_memory  = thresholds.pressure;
	m_critical_gc_memory  = thresholds.critical;
}

void TextureCache::NoteEviction(const Image& image, bool downloaded) {
	const auto bytes = image.AccountedSize();
	m_log_evictions++;
	m_log_evicted_bytes += bytes;
	Common::PerfStats::Count(Common::PerfStats::Counter::GcEvictions);
	Common::PerfStats::Count(Common::PerfStats::Counter::GcEvictedBytes, bytes);
	if (downloaded) {
		m_log_downloads++;
		Common::PerfStats::Count(Common::PerfStats::Counter::GcDownloads);
	}
}

void TextureCache::CollectWithinBudget(uint64_t tick, uint64_t used_memory) {
	m_gc_written_cold_bytes  = 0;
	m_gc_written_large_bytes = 0;
	const bool critical  = used_memory >= m_critical_gc_memory;
	const bool pressured = critical || used_memory >= m_pressure_gc_memory;
	const auto age       = std::min(critical ? kAgeCritical : pressured ? kAgePressured : kAgeNormal,
	                                tick);
	const size_t limit = critical ? 40 : pressured ? 20 : 10;

	const auto evictable = [&](ImageId id) -> const Image* {
		const auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || image->depth_id ||
		    image->binding.is_bound || image->binding.is_target) {
			return nullptr;
		}
		return image;
	};

	// Images whose contents guest memory already holds (never GPU-written, or overwritten by the
	// CPU or a buffer since). Collect first: deleting depth also deletes its stencil associations.
	std::vector<ImageId> plain;
	plain.reserve(limit);
	m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
		const auto* image = evictable(id);
		if (image != nullptr && !image->SafeToDownload()) {
			plain.push_back(id);
		}
		return plain.size() == limit;
	});

	uint64_t freed = 0;
	for (const auto id: plain) {
		auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered) {
			continue; // went with an earlier depth image
		}
		freed += image->AccountedSize();
		NoteEviction(*image, false);
		FreeImage(id);
		if (used_memory - std::min(freed, used_memory) < m_trigger_gc_memory) {
			return;
		}
	}
	// Keep authoritative GPU data tracked until its writeback has actually completed.
	// In particular, FreeImage must not untrack it while the priority callback still owns
	// the only up-to-date bytes. One image per pass bounds copy volume and GPU waits.
	if (!pressured || !m_evict_written || !m_scheduler.Active()) return;
	const auto idle = GpuBudget::WrittenIdleFrames(critical);
	if (tick < idle) return;
	const auto& staging     = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	const auto  limit_bytes = GpuBudget::WrittenDownloadLimit(critical, staging.Size());
	const auto  chunk_bytes = std::min(staging.Size(), GpuBudget::kWrittenChunkBytes);
	ImageId     candidate {};
	uint64_t    candidate_bytes = 0;
	m_lru_cache.ForEachItemBelow(tick - idle, [&](ImageId id) {
		const auto* image = evictable(id);
		if (image == nullptr || !image->SafeToDownload() || image->usage.video_out ||
		    image->info.IsDepth() || !image->stencil_ids.empty() || !CanDownload(*image))
			return;
		const auto size = image->info.data.size;
		if (size == 0) return;
		m_gc_written_cold_bytes += image->AccountedSize();
		if (size > limit_bytes) {
			if (!CanDownloadInChunks(*image, chunk_bytes)) {
				m_gc_written_large_bytes += image->AccountedSize();
				return;
			}
		}
		if (id == m_written_window.image && image->ContentVersion() == m_written_window.version) {
			candidate       = id;
			candidate_bytes = UINT64_MAX;
			return;
		}
		// Reclaim a useful amount rather than spending the pass on a tiny old image.
		if (image->AccountedSize() > candidate_bytes) {
			candidate       = id;
			candidate_bytes = image->AccountedSize();
		}
	});
	if (!candidate) return;
	if (m_slot_images[candidate].info.data.size > limit_bytes) {
		if (DownloadImageMemoryChunk(candidate, chunk_bytes)) {
			NoteEviction(m_slot_images[candidate], true);
			FreeImage(candidate);
		}
		return;
	}
	if (!DownloadImageMemory(candidate, false)) return;
	const auto completion_tick = m_scheduler.CurrentTick();
	{
		const Common::PerfStats::ScopedWaitSite wait_site(Common::PerfStats::Wait::SemGc);
		m_scheduler.Wait(completion_tick);
		m_scheduler.WaitPriorityOperations(completion_tick);
	}
	auto* image = m_slot_images.try_get(candidate);
	if (image == nullptr || !image->registered) return;
	NoteEviction(*image, true);
	FreeImage(candidate);
}

} // namespace Libs::Graphics
