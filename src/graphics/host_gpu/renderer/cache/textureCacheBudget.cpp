// --gpu-memory-budget: the texture cache collector when it keeps to one device-memory budget
// (gpuMemoryBudget.h) instead of the legacy heap-size thresholds.

#include "common/emulatorConfig.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"

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
// --gpu-evict-written: GPU-written images (render targets, storage images) are downloaded to guest
// memory before they go: only under pressure, only when long unused, and within one download
// buffer slice per tick.
constexpr uint64_t kDownloadAgePressured = 120;
constexpr uint64_t kDownloadAgeCritical  = 60;
constexpr uint64_t kDownloadBudget       = 32 * GpuBudget::MiB;

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

	// Long-unused GPU-written images, tiled render targets included (the legacy collector never
	// evicts those): typically targets of an earlier scene.
	std::vector<ImageId> written;
	if (pressured && m_evict_written) {
		const auto download_age =
		    std::min(critical ? kDownloadAgeCritical : kDownloadAgePressured, tick);
		uint64_t download_bytes = 0;
		m_lru_cache.ForEachItemBelow(tick - download_age, [&](ImageId id) {
			const auto* image = evictable(id);
			if (image == nullptr || !image->SafeToDownload() || !CanDownload(*image) ||
			    download_bytes + image->info.data.size > kDownloadBudget) {
				return false;
			}
			download_bytes += image->info.data.size;
			written.push_back(id);
			return written.size() == limit / 2;
		});
	}

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
	for (const auto id: written) {
		auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || !DownloadImageMemory(id)) {
			continue;
		}
		NoteEviction(*image, true);
		FreeImage(id);
	}
}

} // namespace Libs::Graphics
