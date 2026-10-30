#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/rpStats.h"
#include "graphics/host_gpu/renderer/pipeline/bindMemo.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"
#include "kernel/addressFold.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "common/processMemory.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/gpuFatal.h"
#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "kernel/addressFold.h"
#include "kernel/memory.h"
#include "libs/audio.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <fmt/format.h>
#include <array>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;

// --perf-stats: why the cache replaces or drops an image while resolving an overlap (each
// replacement is a new native image, uploaded again from guest memory). Logs the first 400
// events with both layouts and a per-reason total every 256 events ([img-recreate]).
void NoteRecreate(const char* reason, const ImageInfo& requested, const ImageInfo& cached) {
	if (!Common::PerfStats::Enabled()) {
		return;
	}
	static std::mutex                               mutex;
	static std::unordered_map<std::string, uint64_t> counts;
	static uint64_t                                 events = 0;
	std::lock_guard lock(mutex);
	counts[reason]++;
	if (++events <= 400) {
		LOGF("[img-recreate] %s addr=0x%010" PRIx64 " req=%ux%ux%u %s tile=%u lv=%u ly=%u size=0x%" PRIx64
		     " cached=0x%010" PRIx64 " %ux%ux%u %s tile=%u lv=%u ly=%u size=0x%" PRIx64 "\n",
		     reason, requested.data.address, requested.extent.width, requested.extent.height,
		     requested.extent.depth, vk::to_string(requested.pixel_format).c_str(),
		     static_cast<uint32_t>(requested.tile_mode), requested.resources.levels,
		     requested.resources.layers, requested.data.size, cached.data.address, cached.extent.width,
		     cached.extent.height, cached.extent.depth, vk::to_string(cached.pixel_format).c_str(),
		     static_cast<uint32_t>(cached.tile_mode), cached.resources.levels, cached.resources.layers,
		     cached.data.size);
	}
	if (events % 256 == 0) {
		std::string line;
		for (const auto& [name, count]: counts) {
			line += fmt::format(" {}={}", name, count);
		}
		LOGF("[img-recreate-sum] events=%" PRIu64 "%s\n", events, line.c_str());
	}
}

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.info.IsDepth()
	                           ? TextureGetSurfaceFormatInfo(desc.info.guest_format).vk_format
	                           : desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	// PS5 DCC fast clears to the CB_COLOR_CLEAR_WORD value fill the keys with 0x10, not the
	// GFX10 0x20.
	if (cmask ? code == 0 : (code == 0x20 || code == 0x10)) {
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid &&
		       DecodePackedColorClear(format, metadata.clear_word, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	// In the 2:10:10:10 layout alpha occupies the low two bits. Integer
	// backing clears must carry the complete guest word, not float channels.
	if (desc.info.guest_format == Prospero::BufferFormat::k2_10_10_10UNorm) {
		clear.uint32[0] = ((code & 0x80u) != 0 ? 0xfffffffcu : 0u) |
		                  ((code & 0x40u) != 0 ? 3u : 0u);
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

} // namespace

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()) {
	if (m_graphics.MemoryBudgetEnabled()) {
		m_budget_mode   = true;
		// My First Gran Turismo can retain over 1 GiB of GPU-written images
		// while Android MemAvailable drops below 200 MiB. The existing written
		// image collector preserves their contents through bounded readback
		// before freeing the Vulkan allocation. Enable it only for PPSA24156;
		// keep every other title's default unchanged. This does not alter the
		// tessellation shaders or pipeline creation path.
		std::string title_id;
		const bool gt_title = Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) &&
		                      title_id == "PPSA24156";
		m_gt_prealloc_reclaim = gt_title;
		const bool gt_written_reclaim = gt_title && [] {
			const char* disabled = std::getenv("KYTY_GT_DISABLE_WRITTEN_RECLAIM");
			return disabled == nullptr || std::strcmp(disabled, "1") != 0;
		}();
		m_evict_written = Config::GpuEvictWrittenEnabled() || gt_written_reclaim;
		if (gt_title) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GT written image reclaim: enabled={} requested_by_config={} (chunked readback; cold-only)\n",
			    m_evict_written ? 1 : 0, Config::GpuEvictWrittenEnabled() ? 1 : 0));
		}
		ApplyBudgetThresholds(m_graphics.MemoryBudget());
	} else if (m_graphics.CanReportMemoryUsage()) {
		constexpr int64_t GiB = 1024ll * 1024 * 1024;
		const auto        budget =
		    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
		const auto threshold = std::min<int64_t>(budget, 8 * GiB);
		m_pressure_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - GiB), GiB + GiB / 2));
		m_critical_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - GiB / 2), 3 * GiB));
		m_trigger_gc_memory = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	}
}

TextureCache::~TextureCache() {
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (cached.GetColorTransform() != requested.GetColorTransform()) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format) ||
	    cached.type != requested.type) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

ImageId TextureCache::InsertImage(const ImageInfo& info, ImageId keep, bool scaled) {
	// Android's low-memory killer can terminate PPSA24156 before Vulkan ever
	// returns VK_ERROR_OUT_OF_DEVICE_MEMORY. In both observed LOW_MEMORY exits,
	// ~1.8 GiB of new sampled texture backing appeared between collector logs.
	// Run the existing ownership-safe, GPU-synchronised allocation recovery
	// *before* allocating more images, not only after an allocation fails.
	// Check once every 8 insertions (or for a >=32 MiB image); do not sample
	// /proc/meminfo or VMA statistics for each small texture.
	if (m_gt_prealloc_reclaim && m_scheduler.Active() && m_gc_tick >= 3 &&
	    ((++m_gt_prealloc_insertions % 8u) == 0u || info.data.size >= 32u * GpuBudget::MiB)) {
		const auto available = Common::ReadProcessMemory().available;
		if (available && *available < GpuBudget::kGtPreallocLowRam) {
			const uint64_t live = m_graphics.GetLiveDeviceMemory();
			const uint64_t frames_since_attempt =
			    m_gt_prealloc_last_usage == 0 ? 0 :
			    m_gc_tick >= m_gt_prealloc_last_tick
			        ? m_gc_tick - m_gt_prealloc_last_tick : 0;
			if (GpuBudget::ShouldPreemptGtImageAllocation(
			        available, live, m_gt_prealloc_last_usage, frames_since_attempt)) {
				// EvictForAllocation protects this call's keep image, skips current
				// and recently used/bound images, copies authoritative GPU data
				// back before freeing, and waits for Vulkan retirement.
				const bool reclaimed = EvictForAllocation(
                        keep, *available < GpuBudget::kLowWater);
				// Measure growth from the post-reclaim residency, not the old
                // high-water mark. Otherwise repeated bursts in one frame can
                // ratchet the peak upward by 64 MiB on every eviction pass.
                m_gt_prealloc_last_usage = reclaimed ? m_graphics.GetLiveDeviceMemory() : live;
				m_gt_prealloc_last_tick = m_gc_tick;
				static std::atomic_uint32_t reported {0};
				if (reported.fetch_add(1, std::memory_order_relaxed) < 180u) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "GT prealloc reclaim: host_available={} MiB used_before={} MiB "
					    "reclaimed={} gc_tick={}\n",
					    *available / GpuBudget::MiB, live / GpuBudget::MiB,
					    reclaimed ? 1u : 0u, m_gc_tick));
				}
			}
		}
	}
	auto id = m_slot_images.insert(m_graphics, m_scheduler, info, scaled);
	for (int attempt = 0; m_slot_images[id].allocation_failed; attempt++) {
		m_slot_images.erase(id);
		if (attempt == 3 || !EvictForAllocation(keep)) {
			LOGF("failed to create image after %d eviction passes: extent=%ux%ux%u format=%u "
			     "layers=%u levels=%u\n",
			     attempt, info.extent.width, info.extent.height, info.extent.depth,
			     static_cast<uint32_t>(info.pixel_format), info.resources.layers,
			     info.resources.levels);
			m_graphics.LogMemoryBudget();
			GpuFatal("vmaCreateImage", vk::Result::eErrorOutOfDeviceMemory);
		}
		id = m_slot_images.insert(m_graphics, m_scheduler, info, scaled);
	}
	if (!info.data.Empty()) {
		RegisterImage(id);
	}
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		m_image_page_table[page].push_back(id);
	});
	image.registered = true;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	if (m_written_window.image == id) m_written_window = {};
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id) {
	auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) {
		return;
	}
	if (image->depth_id) {
		std::erase(m_slot_images[image->depth_id].stencil_ids, id);
	} else {
		// Detach first: deleting each association also unlinks it from its depth owner.
		for (const auto association: std::exchange(image->stencil_ids, {})) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.HasMetadata()) {
		const auto metadata = m_surface_metas.find(image->info.metadata.range.address);
		if (metadata != m_surface_metas.end() &&
		    image->info.metadata.kind == ImageMetadataKind::Htile &&
		    metadata->second.type == MetaDataInfo::Type::HTile) {
			// A later binding may have reused this address for another metadata type.
			m_surface_metas.erase(metadata);
		}
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.IsGpuModified()) {
		image.ClearGpuModified();
	}
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	if (m_written_window.image && m_slot_images.try_get(m_written_window.image) == &image)
		m_written_window = {};
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	image.track_addr   = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	image.track_addr_end = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	if (m_readback_linear_images && !image.info.IsTiled() && !image.info.data.Empty()) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

ImageId TextureCache::GetNullImage(const ImageDesc& desc) {
	const auto format = desc.info.pixel_format;
	if (const auto found = m_null_images.find(format); found != m_null_images.end()) {
		return found->second;
	}
	ImageInfo info {};
	info.pixel_format    = desc.info.pixel_format;
	info.guest_format    = desc.info.guest_format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = std::max(desc.info.bytes_per_block, 1u);
	info.samples         = 1;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.mip_layout[0]   = {0, info.bytes_per_block, 1, 1};
	const auto id        = InsertImage(info);
	m_null_images.emplace(format, id);
	return id;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::MatchScale(Image& first, Image& second) {
	if (first.scaled == second.scaled) {
		return;
	}
	auto& scaled   = first.scaled ? first : second;
	auto& unscaled = first.scaled ? second : first;
	if (unscaled.CanScale()) {
		unscaled.SetScaled(true);
	} else {
		scaled.SetScaled(false);
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	// Guest D16 may be represented as two-byte depth/color on one side and
	// four-byte D24/D32 host depth on the other. Detect by actual transfer
	// bytes rather than IsDepth(), which misses a D16-depth to D32-depth copy.
	const bool source_wide = DepthAspectTransferBytes(source.backing.format) == sizeof(uint32_t);
	const bool dest_wide = DepthAspectTransferBytes(destination.backing.format) == sizeof(uint32_t);
	if (source_wide == dest_wide) {
		return false;
	}
	auto& wide   = source_wide ? source : destination;
	auto& narrow = source_wide ? destination : source;
	if (wide.info.bytes_per_block != sizeof(uint16_t) ||
	    narrow.info.bytes_per_block != sizeof(uint16_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto layers = wide.info.resources.layers;
	const uint64_t wide_slice = static_cast<uint64_t>(wide.info.pitch) *
	                            wide.info.extent.height * sizeof(uint32_t);
	const uint64_t narrow_slice = static_cast<uint64_t>(narrow.info.pitch) *
	                              narrow.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || wide_slice > UINT64_MAX / layers ||
	        narrow_slice > UINT64_MAX / layers);

	const std::array wide_copies {
	    vk::BufferImageCopy {0,
	                         wide.info.pitch,
	                         wide.info.extent.height,
	                         {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                         {},
	                         wide.info.extent}};
	const std::array narrow_copies {
	    vk::BufferImageCopy {0,
	                         narrow.info.pitch,
	                         narrow.info.extent.height,
	                         {narrow.info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
	                                                : vk::ImageAspectFlagBits::eColor,
	                          0, 0, layers},
	                         {},
	                         narrow.info.extent}};

	auto wide_buffer = m_tiler.GetScratchBuffer(wide_slice * layers);
	auto narrow_buffer = m_tiler.GetScratchBuffer(narrow_slice * layers, wide_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = wide.info.extent.width,
	    .height              = wide.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(narrow.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(wide.info.pitch) * sizeof(uint32_t),
	    .source_slice_stride = narrow_slice,
	    .target_slice_stride = wide_slice,
	};
	const bool d32 = DepthAspectTransferFormat(wide.backing.format) == vk::Format::eD32Sfloat;
	if (source_wide) {
		source.Download(wide_copies, wide_buffer.buffer, wide_buffer.offset, wide_buffer.size);
		m_tiler.ConvertD16(wide_buffer, narrow_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(narrow_copies, narrow_buffer.buffer, narrow_buffer.offset,
		                   narrow_buffer.size);
	} else {
		source.Download(narrow_copies, narrow_buffer.buffer, narrow_buffer.offset,
		                narrow_buffer.size);
		m_tiler.ConvertD16(narrow_buffer, wide_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(wide_copies, wide_buffer.buffer, wide_buffer.offset,
		                   wide_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    source.info.GetColorTransform() == destination.info.GetColorTransform() &&
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	if (direct_copy) {
		MatchScale(source, destination);
		destination.CopyImage(source);
	} else {
		// Conversion copies address guest texels of both images.
		source.SetScaled(false);
		destination.SetScaled(false);
		if (!CopyD16(destination, source)) {
			if (source.backing.samples != 1 || destination.backing.samples != 1) {
				EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
			}
			auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
			destination.CopyImageWithBuffer(source, copy_buffer, m_tiler);
		}
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	if (m_slot_images[destination_id].IsBufferModified()) {
		RefreshImage(destination_id);
	}
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	// Mip copies address guest texels of both images.
	source.SetScaled(false);
	destination.SetScaled(false);
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264
	const bool raw_d16_texture =
	    binding == BindingType::Texture && cached.info.IsDepth() &&
	    cached.info.guest_format == Prospero::BufferFormat::k16UNorm &&
	    requested.guest_format == Prospero::BufferFormat::k16UInt &&
	    requested.pixel_format == vk::Format::eR16Uint && cached.backing.samples == 1 &&
	    requested.samples == 1 && requested.data == cached.info.data &&
	    requested.extent == cached.info.extent && requested.resources == cached.info.resources &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	// PPSA04264, PPSA04288
	// A partial view retains the entire matching array layout. HTile belongs to
	// the depth source, independently of the data slices copied into color storage.
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    (cached.info.metadata.kind == ImageMetadataKind::None ||
	     cached.info.metadata.kind == ImageMetadataKind::Htile);
	bool recreate = cached.info.resources < requested.resources ||
	                requested.IsVolume() != cached.info.IsVolume();
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_d16_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout          = cached.info.mip_layout;
		info.tiling              = cached.info.tiling;
		info.tiled_slice_stride  = cached.info.tiled_slice_stride;
		info.linear_slice_stride = cached.info.linear_slice_stride;
		info.first_tail_level    = cached.info.first_tail_level;
	}
	info.htile_initial_clear  = false;
	const auto replacement_id = InsertImage(info, cached_id);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		cached.SetScaled(false);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	NoteRecreate("depth-replace", requested, cached.info);
	FreeImage(cached_id);
	return replacement_id;
}

namespace BcAlias {

bool Matches(const ImageInfo& requested, const ImageInfo& cached) {
	return requested.guest_format != cached.guest_format &&
	       requested.IsBlock() != cached.IsBlock() &&
	       requested.tile_mode == cached.tile_mode &&
	       requested.bytes_per_block == cached.bytes_per_block &&
	       requested.samples == 1 && cached.samples == 1 &&
	       (requested.data != cached.data || requested.resources != cached.resources ||
	        requested.BlockExtent() != cached.BlockExtent()) &&
	       !TextureCache::SameBacking(cached, requested, false);
}

bool Publish(TextureCache& cache, Image& image) {
	if (!cache.CanDownload(image)) {
		return false;
	}
	// ObtainBuffer preserves padding and records ownership before the GPU-only transfer.
	auto [buffer, offset] =
	    cache.m_buffer_cache.ObtainBuffer(image.info.data.address, image.info.data.size, true);
	cache.DownloadImage(image, *buffer, offset, image.info.data.size, image.info.resources.levels);
	image.ClearGpuModified();
	image.MarkBufferModified();
	return true;
}

void Resolve(TextureCache& cache, ImageId cached_id, ImageId merged_id) {
	auto& cached = cache.m_slot_images[cached_id];
	auto* merged = merged_id ? &cache.m_slot_images[merged_id] : nullptr;
	// PS5 raw block images can span several packed BC mips. Transfer their complete
	// guest bytes, including tail padding, before retiring the incompatible backing.
	if (cached.SafeToDownload()) {
		if (merged && merged->SafeToDownload() && !Publish(cache, *merged)) {
			return;
		}
		if (!Publish(cache, cached)) {
			return;
		}
		if (merged) {
			merged->MarkBufferModified();
		}
	}
	cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
	cache.FreeImage(cached_id);
}

} // namespace BcAlias

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr || !owner->registered) {
		return {merged_id};
	}
	if (BcAlias::Matches(requested, owner->info) && !owner->IsCpuDirty()) {
		BcAlias::Resolve(*this, cached_id, merged_id);
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     !std::equal(requested.mip_layout.begin(), requested.mip_layout.end(),
		                 cached.info.mip_layout.begin(),
		                 [](const ImageMipInfo& left, const ImageMipInfo& right) {
			                 return left.offset == right.offset && left.size == right.size &&
			                        left.pitch == right.pitch && left.height == right.height &&
			                        left.tail_x == right.tail_x && left.tail_y == right.tail_y &&
			                        left.surface_z == right.surface_z;
		                 }))) {
			if (safe_to_delete) {
				NoteRecreate("layout-free", requested, cached.info);
				FreeImage(cached_id);
			}
			return {merged_id};
		}
		if (requested.GetColorTransform() != cached.info.GetColorTransform()) {
			NoteRecreate("expand-color-transform", requested, cached.info);
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			NoteRecreate("expand-block", requested, cached.info);
			return {ExpandImage(requested, cached_id)};
		}
		// Volume depth is not an array-layer count. A larger depth can retain the
		// same block-slice layout while requiring a larger native image.
		if ((requested.IsVolume() || cached.info.IsVolume()) &&
		    (requested.data.size == cached.info.data.size ||
		     (requested.type == cached.info.type && requested.resources == cached.info.resources &&
		      requested.extent.width == cached.info.extent.width &&
		      requested.extent.height == cached.info.extent.height &&
		      requested.extent.depth > cached.info.extent.depth))) {
			NoteRecreate("expand-volume", requested, cached.info);
			return {ExpandImage(requested, cached_id)};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			NoteRecreate("expand-view", requested, cached.info);
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			NoteRecreate("expand-mips", requested, cached.info);
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.type != cached.info.type) {
			return {merged_id};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode));
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			NoteRecreate("expand-mip-of", requested, cached.info);
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		NoteRecreate("merge-mip", requested, cached.info);
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address && safe_to_delete) {
		NoteRecreate("partial-free", requested, cached.info);
		FreeImage(cached_id);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info, source_id, m_slot_images[source_id].scaled);
	auto&      source      = m_slot_images[source_id];
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

bool TextureCache::CanDownload(const Image& image) const {
	const auto& info = image.info;
	if (info.samples != 1 || image.backing.samples != 1 || info.resources.layers == 0 ||
	    info.data.size % info.resources.layers != 0) {
		return false;
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		return IsSupportedDepthPlaneReadback(info) &&
		       Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
	}
	return info.metadata.compression == VideoOutCompression::Uncompressed &&
	       (!info.IsTiled() || !Prospero::IsFmaskTextureFormat(info.guest_format)) &&
	       info.LinearSize() != 0;
}

// --perf-stats: classify uploads of AvPlayer video frames (race with the decoder write, or a
// frame whose sampled luma is entirely black) to pin down black flashes during video playback.
void TextureCache::NoteVideoUpload(const Image& image) {
	const auto address = image.info.data.address;
	const auto size    = image.info.data.size;
	if (!Libs::Audio::AvPlayer::IsVideoBuffer(address, size)) {
		return;
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::VideoUploads);
	static std::atomic_uint32_t s_logged = 0;
	if (Libs::Audio::AvPlayer::VideoBufferBeingWritten(address, size)) {
		Common::PerfStats::Count(Common::PerfStats::Counter::VideoUploadRaces);
		if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("TextureCache: video frame at 0x%016" PRIx64 " uploaded while AvPlayer was still "
			     "writing it\n",
			     address);
		}
	}
	// Sample the first plane (luma for NV12): black video is 16 (limited) or 0 (full range).
	const auto* bytes = reinterpret_cast<const uint8_t*>(address);
	constexpr uint32_t kSamples = 64;
	const auto         span     = std::max<uint64_t>(size * 2 / 3, kSamples);
	bool               black    = true;
	for (uint32_t i = 0; i < kSamples && black; i++) {
		const auto value = bytes[span * i / kSamples];
		black            = value == 16 || value == 0;
	}
	if (black) {
		Common::PerfStats::Count(Common::PerfStats::Counter::VideoBlackUploads);
		if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("TextureCache: video frame at 0x%016" PRIx64 " (%" PRIu64
			     " bytes) uploaded all black\n",
			     address, size);
		}
	}
}

void TextureCache::UploadImage(Image& image, Buffer& source, uint64_t source_offset) {
	Common::PerfStats::Count(Common::PerfStats::Counter::ImageUploadBytes, image.info.data.size);
	if (Common::PerfStats::Enabled()) {
		NoteVideoUpload(image);
	}
	auto& destination = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto& info        = image.info;
	const auto  binding     = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	// Buffer copies address guest texels and overwrite the whole image; a scaled target takes
	// the uploaded contents back up so an attachment bind that refreshes it keeps its pass size.
	const bool was_scaled = destination.scaled;
	destination.SetScaled(false, /*preserve=*/false);
	const auto restore_scale = [&destination, was_scaled] {
		if (was_scaled) {
			destination.SetScaled(true);
		}
	};
	if (binding != BindingType::DepthTarget) {
		if ((binding == BindingType::RenderTarget || binding == BindingType::VideoOut) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		if (binding == BindingType::VideoOut &&
		    info.metadata.compression != VideoOutCompression::Uncompressed) {
			EXIT("TextureCache: invalid video-out upload\n");
		}
		const auto linear = m_tiler.Detile(source.Handle(), source_offset, info);
		const auto copies = info.BufferCopies(linear.offset);
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
		restore_scale();
		return;
	}

	if (info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	const auto layers          = info.resources.layers;
	const auto full_slice_size = info.linear_slice_stride;
	auto       linear          = m_tiler.Detile(source.Handle(), source_offset, info);
	auto       copies          = info.BufferCopies(linear.offset);
	if (image.depth_id) {
		for (auto& copy: copies) {
			copy.imageSubresource.baseArrayLayer += image.stencil_subresources.base_layer;
		}
	}
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		copies.resize(1);
		copies[0].bufferOffset      = linear.offset;
		copies[0].bufferRowLength   = info.pitch;
		copies[0].bufferImageHeight = info.extent.height;
		copies[0].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, 0, layers};
		copies[0].imageExtent       = info.extent;
	}
	destination.Upload(copies, linear.buffer, linear.offset, linear.size);
	restore_scale();
}

void TextureCache::InitializeImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.info.data.Empty()) {
		return;
	}
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		return;
	}
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	if (upload) {
		const auto [source, source_offset] =
		    m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
		if (source == nullptr) {
			EXIT("TextureCache: failed to obtain image upload source\n");
		}
		UploadImage(image, *source, source_offset);
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

// Whether every byte of data equals code, comparing eight bytes at a time.
static bool UniformBytes(const uint8_t* data, size_t size, uint8_t code) {
	const uint64_t pattern = uint64_t {code} * 0x0101010101010101ull;
	size_t         i       = 0;
	for (; i + sizeof(uint64_t) <= size; i += sizeof(uint64_t)) {
		uint64_t word = 0;
		std::memcpy(&word, data + i, sizeof(word));
		if (word != pattern) {
			return false;
		}
	}
	for (; i < size; i++) {
		if (data[i] != code) {
			return false;
		}
	}
	return true;
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& desc,
                                       uint32_t metadata_base_layer) {
	if (desc.info.metadata.kind != ImageMetadataKind::Dcc &&
	    desc.info.metadata.kind != ImageMetadataKind::Cmask) {
		return;
	}
	const auto range = desc.info.metadata.range;
	{
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// Native color metadata must not retain a reused HTile/CMask/FMask clear flag.
		m_surface_metas.erase(range.address);
		if (range.size != 0) {
			auto& known = m_color_metadata_ranges[range.address];
			known       = std::max(known, range.size);
		}
		if (range.size == 0 || desc.info.resources.levels != 1) {
			return;
		}
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks.
	constexpr uint64_t MetadataBlockSize = 0x1000;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	// Finish native metadata writes before reading backing bytes. This can submit the scheduler,
	// so discovery runs before final draw uploads and never holds the texture lock across it.
	if (m_buffer_cache.IsRegionGpuModified(range.address, range.size)) {
		const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemMetadata);
		m_buffer_cache.ReadMemory(range.address, range.size, false);
	}
	const auto slice_size = range.size / layers;
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		uint8_t code = 0;
		if (!LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			EXIT("TextureCache: failed to read color metadata backing\n");
		}
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		std::vector<uint8_t> bytes(slice_size);
		if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
			EXIT("TextureCache: failed to read color metadata slice\n");
		}
		// bind-memo bit ColorClearScan: the same test eight bytes at a time.
		const bool uniform =
		    (BindMemo::Bits() & BindMemo::ColorClearScan) != 0
		        ? UniformBytes(bytes.data(), bytes.size(), code)
		        : std::all_of(bytes.begin(), bytes.end(), [code](uint8_t byte) { return byte == code; });
		if (!uniform) {
			continue;
		}
		{
			std::scoped_lock lock {m_lock};
			const auto& image = m_slot_images[id];
			const bool depth = image.info.IsDepth();
			if (depth) {
				const auto value = clear.color.float32[0];
				clear.depthStencil = {value, 0u};
			}
			ClearImage(m_scheduler.Current(), id, depth ? image.backing.format : view.format,
			           {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
			            view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		// Publish the conversion's expanded keys without treating them as guest writes
		// to overlapping image data. Invalidate the buffer before updating its backing.
		if (desc.type != BindingType::VideoOut) {
			std::fill(bytes.begin(), bytes.end(), uint8_t {0xff});
			m_buffer_cache.InvalidateMemory(address, slice_size);
			LibKernel::Memory::WriteBacking(address, bytes.data(), bytes.size());
			WalkFan::NoteBackingWrite(Kernel::AddressFold::Fold(address), bytes.size());
		}
	}
}

// --perf-stats: which images are re-uploaded from guest memory, and why. Logs an image on its
// 1st, 2nd, 4th, 8th, ... refresh (at most 600 lines), so images refreshed every frame stand out.
// src= / bsrc= name what made the guest bytes newer (RefreshCause). Every 256 refreshes an
// [img-refresh-sum] line totals the refreshed bytes by source and by image kind.
static void NoteImageRefresh(const Image& image, bool buffer_modified, bool cpu_dirty) {
	static std::mutex                             mutex;
	static std::unordered_map<uint64_t, uint32_t> counts;
	static uint32_t                               logged = 0;
	static uint64_t                               refreshes = 0;
	static std::array<uint64_t, static_cast<size_t>(RefreshCause::Source::Count)> bytes_by_source {};
	static uint64_t                               rt_bytes = 0;
	static uint64_t                               total_bytes = 0;
	std::lock_guard lock(mutex);
	const auto source = buffer_modified ? image.buffer_source : image.dirty_source;
	bytes_by_source[static_cast<size_t>(source)] += image.info.data.size;
	total_bytes += image.info.data.size;
	if (image.usage.render_target || image.usage.storage || image.IsGpuModified()) {
		rt_bytes += image.info.data.size;
	}
	if (++refreshes % 256 == 0) {
		std::string line;
		for (size_t i = 0; i < bytes_by_source.size(); i++) {
			line += fmt::format(" {}={}", RefreshCause::Name(static_cast<RefreshCause::Source>(i)),
			                    bytes_by_source[i] >> 20u);
		}
		LOGF("[img-refresh-sum] refreshes=%" PRIu64 " MiB=%" PRIu64 " (target/storage %" PRIu64
		     ") by source:%s\n",
		     refreshes, total_bytes >> 20u, rt_bytes >> 20u, line.c_str());
	}
	const auto      n = ++counts[image.info.data.address];
	if (!std::has_single_bit(n) || logged >= 600) {
		return;
	}
	logged++;
	LOGF("[img-refresh] addr=0x%010" PRIx64 " size=0x%" PRIx64 " %ux%ux%u fmt=%s tile=%u n=%u"
	     " buffer_modified=%d cpu_dirty=%d src=%s bsrc=%s rt=%d storage=%d\n",
	     image.info.data.address, image.info.data.size, image.info.extent.width,
	     image.info.extent.height, image.info.extent.depth,
	     vk::to_string(image.info.pixel_format).c_str(),
	     static_cast<uint32_t>(image.info.tile_mode), n, buffer_modified ? 1 : 0, cpu_dirty ? 1 : 0,
	     RefreshCause::Name(image.dirty_source), RefreshCause::Name(image.buffer_source),
	     image.usage.render_target ? 1 : 0, image.usage.storage ? 1 : 0);
}

void TextureCache::RefreshImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		(void)image.ResolveMaybeCpuHash(image.HashGuestEdges());
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	if (Common::PerfStats::Enabled()) {
		NoteImageRefresh(image, image.IsBufferModified(), image.IsDefinitelyCpuDirty());
	}
	InitializeImage(id);
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info, depth_id);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	if (record.depth_id != depth_id || record.stencil_subresources != depth.stencil_subresources) {
		ImageInfo plane {};
		plane.data         = stencil;
		plane.pixel_format = vk::Format::eS8Uint;
		plane.guest_format = Prospero::BufferFormat::k8UInt;
		plane.type         = depth.info.type;
		plane.extent       = depth.info.extent;
		plane.resources    = {depth.stencil_subresources.level_count,
		                      depth.stencil_subresources.layer_count};
		plane.tile_mode    = depth.info.tile_mode;
		plane.pitch =
		    plane.IsTiled() ? TileGetDepthPitch(plane.extent.width, 1, 0) : depth.info.pitch;
		plane.bytes_per_block = 1;
		plane.UpdateSize();
		if (record.depth_id != depth_id) {
			if (record.depth_id) {
				std::erase(m_slot_images[record.depth_id].stencil_ids, association);
			}
			depth.stencil_ids.push_back(association);
		}
		record.info                 = plane;
		record.depth_id             = depth_id;
		record.stencil_subresources = depth.stencil_subresources;
	}

	return association;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	if (desc.info.data.Empty()) {
		std::scoped_lock lock {m_lock};
		return GetNullImage(desc);
	}
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		std::scoped_lock lock {m_lock};
		const auto       candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);
		bool block_alias = false;

		for (const auto id: candidates) {
			const auto& image = m_slot_images[id];
			// Stencil descriptors name the associated plane; its depth owner supplies
			// the native image and the canonical guest tiling.
			if ((desc.type == BindingType::Texture || desc.type == BindingType::Storage) &&
			    image.depth_id && image.info.data == desc.info.data &&
			    m_slot_images[image.depth_id].info.samples == desc.info.samples &&
			    image.info.extent == desc.info.extent && image.info.type == desc.info.type &&
			    image.info.resources.levels >= desc.info.resources.levels &&
			    image.info.resources.layers >= desc.info.resources.layers &&
			    desc.info.bytes_per_block == 1 &&
			    desc.info.guest_format == Prospero::BufferFormat::k8UInt) {
				result = id;
				break;
			}
			if (SameBacking(image.info, desc.info, exact_format)) {
				result = id;
			} else if (image.SafeToDownload() &&
			           BcAlias::Matches(desc.info, image.info)) {
				block_alias = true;
			}
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				view_mip                = -1;
				view_layer              = -1;
				const auto& merged_info = result ? m_slot_images[result].info : desc.info;
				const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
				}
			}
		} else if (block_alias) {
			for (const auto candidate: candidates) {
				const auto* owner = m_slot_images.try_get(candidate);
				if (candidate != result && owner != nullptr && owner->registered &&
				    !owner->IsCpuDirty() &&
				    BcAlias::Matches(m_slot_images[result].info, owner->info)) {
					BcAlias::Resolve(*this, candidate, result);
				}
			}
		}

		if (result && !m_slot_images[result].depth_id) {
			auto& resolved = m_slot_images[result];
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				NoteRecreate("fewer-resources", desc.info, resolved.info);
				FreeImage(result);
				result = {};
			}
		}
		if (!result) {
			const bool target = desc.type == BindingType::RenderTarget ||
			                    desc.type == BindingType::DepthTarget;
			result         = InsertImage(desc.info, {}, target);
			auto& inserted = m_slot_images[result];
			if (Common::PerfStats::Enabled()) {
				const Image* overlapping = nullptr;
				for (const auto candidate: candidates) {
					const auto* owner = m_slot_images.try_get(candidate);
					if (owner != nullptr && candidate != result) {
						overlapping = owner;
						break;
					}
				}
				NoteRecreate(overlapping == nullptr ? "insert-fresh" : "insert-overlapping", desc.info,
				             overlapping == nullptr ? desc.info : overlapping->info);
			}
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified();
			}
		}
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	MaterializeColorClear(result, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[result];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
	return result;
}

void TextureCache::UpdateImage(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = m_slot_images.try_get(owner->depth_id);
		}
		if (owner == nullptr || (ensure_valid && !owner->SafeToDownload())) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected && ensure_valid) {
		const auto owner = m_slot_images.try_get(selected);
		if (owner != nullptr && owner->depth_id) {
			selected = owner->depth_id;
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	if (!image.info.data.Empty()) {
		if (!image.registered || image.depth_id || image.binding.needs_rebind) {
			EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
		}
	}
	if (desc.type == BindingType::Storage) {
		image.MarkGpuModified();
	}
	if (!image.info.data.Empty()) {
		RefreshImage(id);
		if (image.info.HasStencil() &&
		    desc.info.data.address >= image.info.stencil.address &&
		    desc.info.data.End() <= image.info.stencil.End()) {
			for (const auto stencil_id:
			     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
				const auto* stencil = m_slot_images.try_get(stencil_id);
				if (stencil != nullptr && stencil->depth_id == id &&
				    stencil->info.data == image.info.stencil) {
					RefreshImage(stencil_id);
					break;
				}
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			if (!image.info.data.Empty()) {
				CommitGpuWrite(image);
			}
			TrackImageDownload(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.render_target = true;
	RefreshImage(id);
	CommitGpuWrite(image);
	TrackImageDownload(id, image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.MarkGpuModified();
	if (desc.info.HasStencil()) {
		const auto layers = image.info.resources.layers;
		EXIT_IF(layers == 0 || image.info.data.size % layers != 0 ||
		        desc.info.data.address < image.info.data.address);
		const auto slice_size = image.info.data.size / layers;
		const auto offset     = desc.info.data.address - image.info.data.address;
		EXIT_IF(slice_size == 0 || offset % slice_size != 0 || offset / slice_size >= layers ||
		        desc.info.resources.layers > layers - offset / slice_size);
		image.stencil_subresources = {0, desc.info.resources.levels,
		                             static_cast<uint32_t>(offset / slice_size),
		                             desc.info.resources.layers};
	}
	image.info.stencil = desc.info.stencil;
	image.info.metadata = desc.info.metadata;
	if (desc.info.HasMetadata()) {
		const auto [metadata, inserted] = m_surface_metas.try_emplace(desc.info.metadata.range.address);
		if (inserted) {
			metadata->second.type = MetaDataInfo::Type::HTile;
			if (image.info.htile_initial_clear) {
				metadata->second.clear_mask.set();
			}
		}
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	if (desc.info.HasStencil()) {
		RefreshImage(AssociateStencil(id, desc.info.stencil));
	}
	return image.FindView(desc.view_info);
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	std::scoped_lock      lock {m_lock};
	ImageId               selected {};
	ImageId               stencil_id {};
	vk::ImageAspectFlags  aspect {};
	ImageSubresourceRange subresources;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		ImageSubresourceRange candidate_subresources {
		    0, owner->info.resources.levels, 0, owner->info.TransferLayers()};
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate              = vk::ImageAspectFlagBits::eStencil;
			candidate_id           = owner->depth_id;
			candidate_subresources = owner->stencil_subresources;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           owner->info.data.size == size) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && (selected != candidate_id || subresources != candidate_subresources)) {
			return false;
		}
		selected     = candidate_id;
		stencil_id   = candidate == vk::ImageAspectFlagBits::eStencil ? id : ImageId {};
		aspect       = candidate;
		subresources = candidate_subresources;
	}
	if (!selected) {
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodeColorDwordFill(image.info.pixel_format, packed_clear, clear.color)) {
			return false;
		}
	} else {
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, subresources.base_level, subresources.level_count,
	            subresources.base_layer, subresources.layer_count}, clear);
	if (stencil_id) {
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	auto& image = m_slot_images[id];
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		} else {
			TrackImage(stencil_id);
			CommitGpuWrite(m_slot_images[stencil_id]);
		}
	}
	command.EndRendering(PassEnd::ImageCopy);
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		ImageViewInfo view {};
		view.format = format;
		view.type   = range.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.base_level  = range.baseMipLevel;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;
		view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
		image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		              vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		vk::RenderingAttachmentInfo attachment {};
		attachment.imageView   = image.FindView(view);
		attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.loadOp      = vk::AttachmentLoadOp::eClear;
		attachment.storeOp     = vk::AttachmentStoreOp::eStore;
		attachment.clearValue  = clear;
		vk::RenderingInfo rendering {};
		rendering.renderArea.extent = {
		    std::max(image.backing.extent.width >> range.baseMipLevel, 1u),
		    std::max(image.backing.extent.height >> range.baseMipLevel, 1u)};
		rendering.layerCount           = range.layerCount;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &attachment;
		command.Handle().beginRendering(&rendering);
		command.Handle().endRendering();
		CommitGpuWrite(image);
		NoteClearForRpStats(image, range, full_image);
		return;
	}
	// KYTY_RP_OPS: a full colour clear of a single-level, single-layer image waits for the next
	// pass that binds it over its whole area (loadOp CLEAR); any other use records it first.
	const RpFold::Shape shape {.color_aspect = range.aspectMask == vk::ImageAspectFlagBits::eColor,
	                           .full_image   = full_image,
	                           .levels       = image.info.resources.levels,
	                           .layers       = image.backing.layers,
	                           .samples      = image.info.samples,
	                           .volume       = image.info.IsVolume()};
	if (RpFold::Eligible(shape) && RpStats::FoldEnabled(command.GetGraphics().IsMesaTurnip())) {
		image.rp_fold.Fold(clear.color);
		CommitGpuWrite(image);
		NoteClearForRpStats(image, range, full_image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	CommitGpuWrite(image);
	NoteClearForRpStats(image, range, full_image);
}

// KYTY_RP_STATS: a full colour clear of a single-level, single-layer image is a loadOp CLEAR
// candidate until another use of the image transitions it (Image::Transit).
void TextureCache::NoteClearForRpStats(Image& image, const vk::ImageSubresourceRange& range,
                                       bool full_image) {
	if (!RpStats::Enabled()) return;
	const bool full_color = full_image && range.aspectMask == vk::ImageAspectFlagBits::eColor &&
	                        image.info.resources.levels == 1 && image.backing.layers == 1 &&
	                        image.info.samples == 1;
	RpStats::OnClear(full_color);
	image.rp_clear_pending = full_color;
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size, RefreshCause::Source source) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	std::scoped_lock lock {m_lock};
	InvalidateCpuAliases(address, size, source);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.linear_slice_stride;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			const auto copies = info.BufferCopies(destination_offset);
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
		} else {
			const auto copies = info.BufferCopies();
			m_tiler.TileImage(image, copies, destination.Handle(), destination_offset,
			                  info.data.size);
		}
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	const std::array copies {vk::BufferImageCopy {0,
	                                              info.pitch,
	                                              info.extent.height,
	                                              {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                                              {},
	                                              info.extent}};
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto guest_linear = tiled ? m_tiler.GetScratchBuffer(info.LinearSize(), host_linear.buffer)
	                          : TileManager::Result {destination.Handle(), destination_offset,
	                                                 destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (tiled) {
		m_tiler.Tile(guest_linear.buffer, guest_linear.offset, guest_linear.size,
		             destination.Handle(), destination_offset, info.data.size, info);
	}
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                 uint64_t destination_size, uint32_t levels) {
	if (!CanDownload(image)) {
		EXIT("TextureCache: invalid image download\n");
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::ImageDownloadBytes, destination_size);
	// Buffer copies address guest texels.
	image.SetScaled(false);
	if (UploadBinding(image) == BindingType::DepthTarget) {
		EXIT_IF(destination_size != image.info.data.size || levels != image.info.resources.levels);
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	const auto& info      = image.info;
	const auto  transform = info.GetColorTransform();
	if (info.IsTiled()) {
		const auto copies = info.BufferCopies(0, levels);
		m_tiler.TileImage(image, copies, destination.Handle(), destination_offset, destination_size,
		                  levels);
	} else if (transform != ColorTransform::None) {
		auto       linear = m_tiler.GetScratchBuffer(destination_size);
		const auto copies = info.BufferCopies(0, levels);
		image.Download(copies, linear.buffer, 0, linear.size);
		m_tiler.TransformColor(linear, {destination.Handle(), destination_offset, destination_size},
		                       transform, false);
	} else {
		const auto copies = info.BufferCopies(destination_offset, levels);
		image.Download(copies, destination.Handle(), destination_offset, destination_size);
	}
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto selected = m_texture_cache.FindImageFromRange(vaddr, size);
	if (!selected) {
		return false;
	}

	std::scoped_lock lock {m_texture_cache.m_lock};
	auto& image = m_texture_cache.m_slot_images[selected];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!image.SafeToDownload()) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else if (image.info.IsVolume() || image.info.resources.layers > 1) {
		// Array and volume slices are strided across the full mip chain.
		return false;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	if (!m_texture_cache.CanDownload(image) ||
	    (TextureCache::UploadBinding(image) == TextureCache::BindingType::DepthTarget &&
	     copy_size != image.info.data.size)) {
		return false;
	}
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, levels);
	// The copy rewrites these bytes on the GPU timeline: in-place readbacks wait for it.
	MarkWritten(image.info.data.address, copy_size);
	return true;
}

bool TextureCache::DownloadImageMemory(ImageId id, bool allow_wait) {
	auto& image = m_slot_images[id];
	if (image.depth_id) {
		return false;
	}
	if (!image.SafeToDownload() || !CanDownload(image)) {
		return false;
	}
	const auto range    = image.info.data;
	auto&      download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] =
	    download.Map(range.size, std::max<uint64_t>(image.info.bytes_per_block, 4), allow_wait);
	if (mapped == nullptr) {
		if (!allow_wait) return false;
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) {
		return false;
	}
	download.Flush(offset, range.size);

	DownloadImage(image, download, offset, range.size, image.info.resources.levels);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering(PassEnd::ImageCopy);
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	m_scheduler.DeferPriorityOperation([&download, range, mapped, offset] {
		download.Invalidate(offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
		WalkFan::NoteBackingWrite(Kernel::AddressFold::Fold(range.address), range.size);
	});
	return true;
}

bool TextureCache::CanDownloadInChunks(const Image& image, uint64_t capacity) const {
	if (image.scaled || image.info.IsVolume() || image.info.IsDepth() ||
	    (!image.info.IsTiled() && image.info.GetColorTransform() != ColorTransform::None) ||
	    !CanDownload(image))
		return false;
	for (uint32_t level = 0; level < image.info.resources.levels; ++level) {
		ImageInfo           window;
		vk::BufferImageCopy copy;
		uint32_t            rows = 0;
		if (!image.info.ReadbackChunk(level, image.info.resources.layers - 1, 0, capacity, window,
		                              copy, rows))
			return false;
		const auto height =
		    (uint64_t {image.info.MipExtent(level).height} + image.info.tiling.texel_height - 1) /
		    image.info.tiling.texel_height;
		if (rows < height) {
			const auto alignment = image.info.IsTiled() ? image.info.tiling.block.block_height : 1u;
			const auto last      = static_cast<uint32_t>((height - 1) / alignment * alignment);
			if (!image.info.ReadbackChunk(level, image.info.resources.layers - 1, last, capacity,
			                              window, copy, rows))
				return false;
		}
	}
	return true;
}

// One window per pass. Keep ownership until all windows reach guest backing memory.
// A later CPU/buffer/GPU write or native image use restarts the cursor.
bool TextureCache::DownloadImageMemoryChunk(ImageId id, uint64_t capacity) {
	auto& image = m_slot_images[id];
	if (m_written_window.image != id || m_written_window.version != image.ContentVersion())
		m_written_window = {id, image.ContentVersion(), 0, 0, 0};
	auto&               cursor = m_written_window;
	ImageInfo           window;
	vk::BufferImageCopy copy;
	uint32_t            rows = 0;
	if (!image.info.ReadbackChunk(cursor.level, cursor.layer, cursor.row, capacity, window, copy,
	                              rows)) {
		cursor = {};
		return false;
	}
	auto& download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] =
	    download.Map(window.data.size, std::max<uint64_t>(image.info.bytes_per_block, 4), false);
	if (!mapped) return false;
	download.Commit();
	if (!LibKernel::Memory::TryReadBacking(window.data.address, mapped, window.data.size))
		return false;
	download.Flush(offset, window.data.size);
	const std::array regions {copy};
	if (window.IsTiled()) {
		m_tiler.TileImage(image, regions, download.Handle(), offset, window.data.size, 1, &window);
	} else if (window.GetColorTransform() != ColorTransform::None) {
		auto linear = m_tiler.GetScratchBuffer(window.data.size);
		image.Download(regions, linear.buffer, 0, linear.size);
		m_tiler.TransformColor(linear, {download.Handle(), offset, window.data.size},
		                       window.GetColorTransform(), false);
	} else {
		copy.bufferOffset = offset;
		const std::array direct {copy};
		image.Download(direct, download.Handle(), offset, window.data.size);
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::ImageDownloadBytes, window.data.size);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = window.data.size;
	m_scheduler.EndRendering(PassEnd::ImageCopy);
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	m_scheduler.DeferPriorityOperation([&download, range = window.data, mapped, offset] {
		download.Invalidate(offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
		WalkFan::NoteBackingWrite(Kernel::AddressFold::Fold(range.address), range.size);
	});
	const auto completion_tick = m_scheduler.CurrentTick();
	{
		const Common::PerfStats::ScopedWaitSite wait_site(Common::PerfStats::Wait::SemGc);
		m_scheduler.Wait(completion_tick);
		m_scheduler.WaitPriorityOperations(completion_tick);
	}
	++m_log_writeback_chunks;
	m_log_writeback_chunk_bytes += window.data.size;
	cursor.row += rows;
	const auto height = (uint64_t {image.info.MipExtent(cursor.level).height} +
	                     image.info.tiling.texel_height - 1) /
	                    image.info.tiling.texel_height;
	if (cursor.row >= height) {
		cursor.row = 0;
		if (++cursor.layer == image.info.resources.layers) {
			cursor.layer = 0;
			++cursor.level;
		}
	}
	if (cursor.level != image.info.resources.levels) return false;
	cursor = {};
	return true;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size,
                                           RefreshCause::Source source) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (image.info.data.address != address) {
			continue;
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		if (!image.IsBufferModified()) {
			image.buffer_source = source;
		}
		image.MarkBufferModified();
	}
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size,
                                        RefreshCause::Source source) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (!owner->IsCpuDirty()) {
			owner->dirty_source = source;
		}
		if (owner->Overlaps(address, size)) {
			owner->MarkCpuDirty();
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

bool TextureCache::IsColorMetadataRange(uint64_t address, uint64_t size) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	std::scoped_lock lock {m_lock};
	auto             next = m_color_metadata_ranges.upper_bound(address);
	if (next == m_color_metadata_ranges.begin()) {
		return false;
	}
	const auto& [start, length] = *std::prev(next);
	return address - start <= length && size <= length - (address - start);
}

bool TextureCache::IsMeta(uint64_t address) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	return found != m_surface_metas.end();
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= found->second.clear_mask.size()) {
		return false;
	}
	return found->second.clear_mask.test(slice);
}

bool TextureCache::ClearMeta(uint64_t address) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask.set();
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= found->second.clear_mask.size()) {
		return false;
	}
	found->second.clear_mask.set(slice, is_clear);
	return true;
}

bool TextureCache::IsRegionRegistered(uint64_t address, uint64_t size) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	std::scoped_lock lock {m_lock};
	return !FindImagesInRegion(address, size, false).empty();
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	address = Kernel::AddressFold::Fold(address); // guest -> host (kernel/addressFold.h)
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
	}
}

void TextureCache::TrimGtInactiveBackings() {
	if (!m_gt_prealloc_reclaim || !m_scheduler.Active() || m_gc_tick < 12 ||
	    (m_gt_last_alternate_trim && m_gc_tick - m_gt_last_alternate_trim < 12))
		return;
	const auto available = Common::ReadProcessMemory().available;
	if (!available || *available >= GpuBudget::kGtPreallocLowRam)
		return;
	m_gt_last_alternate_trim = m_gc_tick;
	uint64_t bytes_queued = 0;
	uint32_t images_queued = 0;
	// Limit to cold/unbound images: alternate backing is never authoritative,
	// but old cached image views may still be in flight. Image defers Vulkan
	// destruction to after the submission completes, then destroys old views.
	m_lru_cache.ForEachItemBelow(m_gc_tick - 8, [&](ImageId id) {
		auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || image->depth_id ||
		    image->binding.is_bound || image->binding.is_target)
			return false;
		const uint64_t freed = image->ReleaseInactiveBacking();
		if (freed == 0) return false;
		bytes_queued += freed;
		++images_queued;
		return bytes_queued >= 256u * GpuBudget::MiB || images_queued >= 48;
	});
	if (!images_queued) return;
	// Deferred destruction must run before the following texture allocation.
	m_scheduler.Finish();
	Log::WriteToConsoleAndLog(fmt::format(
	    "GT inactive backing reclaim: host_available={} MiB retired={} "
	    "MiB images={} tick={}\n",
	    *available / GpuBudget::MiB, bytes_queued / GpuBudget::MiB,
	    images_queued, m_gc_tick));
}

void TextureCache::LogMemoryBreakdown(uint64_t used_memory) {
	constexpr int64_t LogIntervalNs = 10'000'000'000;
	const auto        now           = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
	if (m_last_memory_log_ns != 0 && now - m_last_memory_log_ns < LogIntervalNs) {
		return;
	}
	m_last_memory_log_ns = now;
	enum Category : uint32_t { Target, Depth, Storage, VideoOut, Sampled, Count };
	std::array<uint64_t, Count> bytes {};
	std::array<uint32_t, Count> images {};
	uint64_t                    guest_bytes = 0;
	uint64_t                    gpu_written_bytes = 0;
	uint64_t                    inactive_bytes = 0;
	uint32_t                    inactive_images = 0;
	m_slot_images.ForEach([&](ImageId, const Image& image) {
		const uint64_t inactive = image.InactiveBackingBytes();
		inactive_bytes += inactive;
		inactive_images += inactive != 0;
		if (image.backing.allocation == nullptr || m_graphics.allocator == nullptr) {
			return;
		}
		VmaAllocationInfo info {};
		vmaGetAllocationInfo(m_graphics.allocator, image.backing.allocation, &info);
		const auto category = image.info.IsDepth()             ? Depth
		                      : image.usage.video_out           ? VideoOut
		                      : image.usage.render_target       ? Target
		                      : image.usage.storage             ? Storage
		                                                        : Sampled;
		bytes[category] += info.size;
		images[category]++;
		guest_bytes += image.AccountedSize();
		if (image.SafeToDownload()) gpu_written_bytes += info.size;
	});
	VmaTotalStatistics stats {};
	if (m_graphics.allocator != nullptr) {
		vmaCalculateStatistics(m_graphics.allocator, &stats);
	}
	uint64_t image_bytes = 0;
	for (const auto value: bytes) {
		image_bytes += value;
	}
	const auto allocated = stats.total.statistics.allocationBytes;
	constexpr uint64_t MiB = 1024ull * 1024;
	const auto device_usage =
	    m_graphics.CanReportMemoryUsage() ? m_graphics.GetDeviceMemoryUsage() : used_memory;
	const auto host = Common::ReadProcessMemory();
	// Bounded diagnostic also survives the default Silent printf setting. Do not enable
	// per-call logging merely to see memory pressure; UMA RSS and device usage can overlap.
	Log::WriteToConsoleAndLog(fmt::sprintf(
	    "GPU memory: device_usage=%" PRIu64 " gc_used=%" PRIu64 " budget=%" PRIu64
	    " MiB | evicted %" PRIu64 " images %" PRIu64 " MiB (%" PRIu64
	    " downloaded) | buffers=%" PRIu64 " MiB mesh_ring=%" PRIu64 " MiB | vma_blocks=%" PRIu64
	    " vma_allocs=%" PRIu64 " MiB | images=%" PRIu64 " MiB (rt %u:%" PRIu64 " depth %u:%" PRIu64
	    " storage %u:%" PRIu64 " videoout %u:%" PRIu64 " sampled %u:%" PRIu64
	    ") guest_accounted=%" PRIu64 " | non_image=%" PRIu64
	    " MiB | metas=%zu | gc trigger/pressure/critical=%" PRIu64 "/%" PRIu64 "/%" PRIu64
	    " MiB | gpu_written_images=%" PRIu64 " MiB evict_written=%u | host_available=%" PRIu64
	    " rss=%" PRIu64 " anon=%" PRIu64 " swap=%" PRIu64
	    " MiB host_fields=%u | cold_written=%" PRIu64 " MiB oversize_written=%" PRIu64
	    " MiB | writeback_chunks=%" PRIu64 " chunk_bytes=%" PRIu64 " MiB\n",
	    device_usage / MiB, used_memory / MiB, m_graphics.MemoryBudget() / MiB, m_log_evictions,
	    m_log_evicted_bytes / MiB, m_log_downloads, m_buffer_cache.TotalUsedMemory() / MiB,
	    m_buffer_cache.MeshRingCapacity() / MiB, stats.total.statistics.blockBytes / MiB,
	    allocated / MiB, image_bytes / MiB, images[Target], bytes[Target] / MiB, images[Depth],
	    bytes[Depth] / MiB, images[Storage], bytes[Storage] / MiB, images[VideoOut],
	    bytes[VideoOut] / MiB, images[Sampled], bytes[Sampled] / MiB, guest_bytes / MiB,
	    (allocated > image_bytes ? allocated - image_bytes : 0) / MiB, m_surface_metas.size(),
	    m_trigger_gc_memory / MiB, m_pressure_gc_memory / MiB, m_critical_gc_memory / MiB,
	    gpu_written_bytes / MiB, m_evict_written ? 1u : 0u, host.available.value_or(0) / MiB,
	    host.resident.value_or(0) / MiB, host.anonymous.value_or(0) / MiB,
	    host.swap.value_or(0) / MiB,
	    (host.available ? 1u : 0u) | (host.resident ? 2u : 0u) | (host.anonymous ? 4u : 0u) |
	        (host.swap ? 8u : 0u),
	    m_gc_written_cold_bytes / MiB, m_gc_written_large_bytes / MiB, m_log_writeback_chunks,
	    m_log_writeback_chunk_bytes / MiB));
	if (m_gt_prealloc_reclaim) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT image backing breakdown: active={} MiB inactive={} MiB ({} images) "
		    "other_vma={} MiB\n",
		    image_bytes / MiB, inactive_bytes / MiB, inactive_images,
		    (allocated > image_bytes + inactive_bytes
		         ? allocated - image_bytes - inactive_bytes : 0) / MiB));
	}
	// At most once per ten seconds: identify why the surviving GPU-written images
	// cannot be reclaimed. Count guest-accounted bytes, separate from VMA usage.
	// This only runs for the title-scoped opt-in, and never changes image lifetime.
	if (m_budget_mode && m_evict_written && m_scheduler.Active()) {
		const uint64_t idle = GpuBudget::WrittenIdleFrames(used_memory >= m_critical_gc_memory);
		if (m_gc_tick >= idle) {
			uint64_t old_written = 0, bound = 0, linked = 0, special = 0, unsupported = 0;
			uint64_t eligible = 0;
			m_lru_cache.ForEachItemBelow(m_gc_tick - idle, [&](ImageId id) {
				const auto* image = m_slot_images.try_get(id);
				if (image == nullptr || !image->registered || !image->SafeToDownload()) return;
				const uint64_t bytes = image->AccountedSize();
				old_written += bytes;
				if (image->depth_id) linked += bytes;
				else if (image->binding.is_bound || image->binding.is_target) bound += bytes;
				else if (image->usage.video_out || image->info.IsDepth() ||
				         !image->stencil_ids.empty()) special += bytes;
				else if (!CanDownload(*image)) unsupported += bytes;
				else eligible += bytes;
			});
			Log::WriteToConsoleAndLog(fmt::sprintf(
			    "GPU written residency: old=%" PRIu64 " bound=%" PRIu64
			    " linked=%" PRIu64 " special=%" PRIu64 " unsupported=%" PRIu64
			    " eligible=%" PRIu64 " MiB (guest-accounted; idle=%" PRIu64 " frames)\n",
			    old_written / MiB, bound / MiB, linked / MiB, special / MiB,
			    unsupported / MiB, eligible / MiB, idle));
		}
	}
	m_log_evictions     = 0;
	m_log_evicted_bytes = 0;
	m_log_downloads     = 0;
	m_log_writeback_chunks = m_log_writeback_chunk_bytes = 0;
}

void TextureCache::RunGarbageCollector() {
	std::scoped_lock lock {m_lock};
	const uint64_t   tick = m_gc_tick++;
	if (m_budget_mode) {
		ApplyBudgetThresholds(m_graphics.UpdateMemoryBudget());
		// Live allocations: free space inside VMA blocks is not something eviction can return.
		const auto used_memory = m_graphics.GetLiveDeviceMemory();
		LogMemoryBreakdown(used_memory);
		TrimGtInactiveBackings();
		const auto live_after_trim = m_graphics.GetLiveDeviceMemory();
		if (live_after_trim >= m_trigger_gc_memory) {
			CollectWithinBudget(tick, live_after_trim);
		}
		return;
	}
	// Driver usage includes other caches and allocator reservations. Keep it
	// separate from the owned-byte counter decremented during unregistration.
	auto used_memory = m_graphics.CanReportMemoryUsage()
	    ? m_graphics.GetDeviceMemoryUsage() : m_total_used_memory;
	LogMemoryBreakdown(used_memory);
	if (used_memory < m_trigger_gc_memory) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && used_memory >= m_critical_gc_memory;
		const uint64_t age       = std::min<uint64_t>(aggressive ? 160 : pressured ? 80 : 16, tick);
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
		std::vector<ImageId> candidates;
		candidates.reserve(deletions);
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first.
		m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
			const auto owner = m_slot_images.try_get(id);
			// Protected entries must not consume every pass's candidate budget.
			if (owner != nullptr && owner->registered && !owner->depth_id &&
			    owner->SafeToDownload() && (owner->info.IsTiled() || !pressured)) return false;
			candidates.push_back(id);
			return candidates.size() == deletions;
		});
		for (const auto id: candidates) {
			if (deletions == 0) {
				break;
			}
			--deletions;
			auto owner = m_slot_images.try_get(id);
			if (owner == nullptr || !owner->registered || owner->depth_id) {
				continue;
			}
			if (owner->IsGpuModified()) {
				const bool safe = owner->SafeToDownload();
				if (safe && owner->info.IsTiled()) {
					continue;
				}
				if (safe && !pressured) {
					continue;
				}
				if (safe && !DownloadImageMemory(id)) {
					continue;
				}
			}
			used_memory -= std::min(used_memory, owner->AccountedSize());
			NoteEviction(*owner, owner->IsGpuModified() && owner->SafeToDownload());
			FreeImage(id);
			if (used_memory < m_critical_gc_memory && aggressive) {
				deletions >>= 2;
				aggressive = false;
			}
			if (used_memory < m_pressure_gc_memory && pressured) {
				deletions >>= 1;
				pressured = false;
			}
		}
	};
	collect(false);
	if (used_memory >= m_critical_gc_memory) {
		collect(true);
	}
}

// An image could not be allocated because device memory is full, which the regular collector
// does not prevent when the working set grows quickly (scene transitions on 4-6 GB GPUs). Evict
// images that are neither bound for the draw being prepared, recently used, nor the caller's
// source image, downloading GPU-written ones like the collector does, then finish the GPU work so
// their deferred destruction actually returns the memory before the caller retries. Caller holds
// m_lock; none of the scheduler's deferred operations take it.
bool TextureCache::EvictForAllocation(ImageId keep, bool reclaim_recent_cpu_backed) {
	constexpr uint64_t EVICT_TARGET    = 256ull << 20u;
	constexpr uint64_t DOWNLOAD_BUDGET = 32ull << 20u;
	if (!m_scheduler.Active() || m_gc_tick < 2) {
		return false;
	}
	std::vector<ImageId> victims;
	uint64_t             bytes      = 0;
	uint64_t             downloaded = 0;
	m_lru_cache.ForEachItemBelow(m_gc_tick - 2, [&](ImageId id) {
		const auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || image->depth_id || id == keep ||
		    image->binding.is_bound || image->binding.is_target) {
			return false;
		}
		if (image->IsGpuModified() && image->SafeToDownload()) {
			// Keep each pass within the reusable download buffer.
			if (downloaded + image->info.data.size > DOWNLOAD_BUDGET) {
				return false;
			}
			downloaded += image->info.data.size;
		}
		victims.push_back(id);
		bytes += image->AccountedSize();
		return bytes >= EVICT_TARGET;
	});
    uint64_t recent_bytes = 0;
    if (reclaim_recent_cpu_backed && bytes < EVICT_TARGET) {
        // The GT LOW_MEMORY trace grew ~1.8 GiB of sampled images in tick 2747.
        // A frame-age-only collector can never catch that burst. Unbound plain
        // textures already have authoritative guest bytes, so evict them without
        // a readback, even when used earlier in this frame. Current draw bindings
        // and the insertion/merge source remain protected. FreeImage queues Vulkan
        // destruction, and Finish below waits before the next allocation.
        m_lru_cache.ForEachItemBelow(m_gc_tick, [&](ImageId id) {
            const auto* image = m_slot_images.try_get(id);
            if (image == nullptr || !image->registered || id == keep ||
                image->binding.is_bound || image->binding.is_target ||
                image->IsGpuModified() || image->IsBufferModified() ||
                image->usage.video_out || image->usage.render_target || image->usage.storage ||
                image->info.IsDepth() || image->depth_id || !image->stencil_ids.empty() ||
                std::find(victims.begin(), victims.end(), id) != victims.end()) return false;
            victims.push_back(id);
            const auto size = image->AccountedSize();
            bytes += size; recent_bytes += size;
            return bytes >= EVICT_TARGET;
        });
    }
    if (recent_bytes) {
        static std::atomic_uint32_t reported {0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 180u)
            Log::WriteToConsoleAndLog(fmt::format(
                "GT intra-frame reclaim: tick={} recent_cpu_backed={} MiB candidates={} (bound/target/keep/GPU-written protected)\n",
                m_gc_tick, recent_bytes / GpuBudget::MiB, victims.size()));
    }
	size_t freed = 0;
	for (const auto id: victims) {
		auto& image = m_slot_images[id];
		const bool download = image.IsGpuModified() && image.SafeToDownload();
		if (download && !DownloadImageMemory(id)) {
			continue;
		}
		NoteEviction(image, download);
		FreeImage(id);
		freed++;
	}
	LOGF("TextureCache: out of device memory, evicted %zu images (%" PRIu64 " MiB)\n", freed,
	     bytes >> 20u);
	if (freed == 0) {
		return false;
	}
	m_scheduler.Finish();
	return true;
}

void TextureCache::ProcessDownloadImages() {
	if (!m_readback_linear_images) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
