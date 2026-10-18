#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/passEnd.h"

#include <compare>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
class TileManager;
struct ImageTestAccess;

using ImageId = Common::SlotId;

struct CachedImageView {
	ImageViewInfo info;
	vk::ImageView view = nullptr;
};

struct ImageUsage {
	bool storage       = false;
	bool render_target = false;
	bool video_out     = false;
};

struct ImageBinding {
	vk::ImageLayout  attachment_layout = vk::ImageLayout::eUndefined;
	vk::AccessFlags2 attachment_access;
	bool             is_bound      = false;
	bool             is_target     = false;
	bool             needs_rebind  = false;
	bool             force_general = false;
	bool             shader_write  = false;
};

class Image final {
public:
	// scaled_hint: allocate the backing at the render scale when the image qualifies (an image
	// first seen as a color or depth target). See renderScale.h.
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info,
	      bool scaled_hint = false);
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	// --render-scale. `scalable`: the image qualifies for a scaled backing (frame-sized 2D
	// color/depth target whose format blits). `scaled`: the current backing is at the scaled
	// size. `scale_blocked`: a use that cannot be scaled was seen (texel access from a stage
	// without rescale data, a pass with unscalable attachments); the image stays at guest size.
	[[nodiscard]] bool CanScale() const noexcept { return scalable && !scale_blocked; }
	// Switches between the guest-size and scaled backings (both stay allocated once used, so
	// Vulkan handles and the driver's per-image state survive the switch), blitting the contents
	// across unless `preserve` is false (the caller overwrites the whole image next). Ends the
	// open render pass; views made before the call stay valid until the GPU is done with them.
	void SetScaled(bool want_scaled, bool preserve = true);
	void BlockScaling() {
		SetScaled(false);
		scale_blocked = true;
	}

	[[nodiscard]] vk::ImageView FindView(const ImageViewInfo& view_info);
	// --render-scale: `view`, made on either backing, as the same view on the current one. A
	// SetScaled after a binding resolved its view leaves that view on the other-size backing
	// (alive, but not what the rescale data and layout transitions describe). nullptr if `view`
	// was not made on this image.
	[[nodiscard]] vk::ImageView CurrentBackingView(vk::ImageView view);
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	// Records the barrier to the new state on `command_buffer` (always the primary buffer).
	// Ends the open render pass unless the barrier can run ahead of it
	// (CommandScheduler::CanHoistBarrier); `reason` is what --perf-stats counts the end as.
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, vk::CommandBuffer command_buffer,
	             PassEnd reason = PassEnd::Transit);
	void Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	            uint64_t size);
	void Download(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	              uint64_t size);
	void CopyImage(Image& source);
	void Resolve(Image& source, const ImageSubresourceRange& source_range,
	             const ImageSubresourceRange& destination_range);
	void CopyImageWithBuffer(Image& source, Buffer& buffer, TileManager& tiler);
	void CopyMip(Image& source, uint32_t mip, uint32_t layer);

	void MarkCpuDirty() {
		m_cpu_dirty       = true;
		m_maybe_cpu_dirty = false;
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty || m_maybe_cpu_dirty; }
	[[nodiscard]] bool IsDefinitelyCpuDirty() const { return m_cpu_dirty; }
	[[nodiscard]] bool IsMaybeCpuDirty() const { return m_maybe_cpu_dirty; }
	void               MarkMaybeCpuDirty() {
		if (!IsCpuDirty()) {
			m_maybe_cpu_hash  = HashGuestEdges();
			m_maybe_cpu_dirty = true;
		}
	}
	[[nodiscard]] bool ResolveMaybeCpuHash(uint64_t hash) {
		if (!m_maybe_cpu_dirty || m_cpu_dirty) {
			EXIT("image cannot resolve maybe-dirty hash\n");
		}
		m_maybe_cpu_dirty = false;
		m_cpu_dirty |= hash != m_maybe_cpu_hash;
		return m_cpu_dirty;
	}

	void RefreshComplete() {
		if (!IsCpuDirty()) {
			EXIT("clean image cannot complete a refresh\n");
		}
		m_cpu_dirty       = false;
		m_maybe_cpu_dirty = false;
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept { m_gpu_modified = true; }
	void               ClearGpuModified() noexcept { m_gpu_modified = false; }

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	void               MarkBufferModified() noexcept { m_buffer_modified = true; }
	void               ClearBufferModified() noexcept { m_buffer_modified = false; }

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size,
	                            bool pages = false) const noexcept {
		return pages ? ImagePageRangesOverlap(info.data.address, info.data.size, address, size)
		             : ImageRangeOverlaps(info.data.address, info.data.size, address, size);
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024);
	}
	[[nodiscard]] uint64_t HashGuestEdges() const;

	ImageInfo        info;
	VulkanImage      backing;
	bool             scalable      = false;
	bool             scaled        = false;
	bool             scale_blocked = false;
	vk::Filter       scale_filter  = vk::Filter::eNearest;
	std::vector<CachedImageView> views;
	ImageUsage       usage;
	ImageBinding     binding;
	bool             registered     = false;
	bool                         allocation_failed = false;
	mutable uint32_t query_epoch    = 0;
	uint64_t         track_addr     = 0;
	uint64_t         track_addr_end = 0;
	ImageId          depth_id {};
	// Stencil associations that refer to this depth image.
	std::vector<ImageId> stencil_ids;
	// The current stencil plane's mapping into the depth image, also retained by its association.
	ImageSubresourceRange stencil_subresources;
	uint64_t         tick_accessed_last = 0;
	size_t           lru_id             = 0;
	// CommandBuffer::PassSerial() of the last render pass a draw used this image in.
	uint64_t         pass_serial        = 0;

private:
	friend struct ImageTestAccess;

	[[nodiscard]] static vk::ImageAspectFlags FullAspectMask(vk::Format format) noexcept;
	[[nodiscard]] bool                        CreateBacking(vk::Extent3D extent, VulkanImage& out);
	void                                      SwapBackings();
	[[nodiscard]] static uint32_t             CopyRows(uint64_t row_size, uint32_t rows,
	                                                   uint64_t capacity) noexcept;
	[[nodiscard]] static uint32_t             CopyExtent(uint32_t source, uint32_t destination,
	                                                     uint32_t source_block,
	                                                     uint32_t destination_block) noexcept;
	[[nodiscard]] static std::pair<uint32_t, uint32_t>
	SanitizeCopyLayers(const Image& source, const Image& destination, uint32_t depth);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	// --render-scale: the backing of the other size (and its views) while `backing` is active.
	VulkanImage                  m_alternate_backing;
	std::vector<CachedImageView> m_alternate_views;
	uint64_t          m_maybe_cpu_hash  = 0;
	bool              m_cpu_dirty       = false;
	bool              m_maybe_cpu_dirty = false;
	bool              m_gpu_modified    = false;
	bool              m_buffer_modified = false;
};

namespace ImageOps {

void                                 Validate(const ImageInfo& info);
[[nodiscard]] Prospero::BufferFormat RenderTargetTransferFormat(uint32_t bytes_per_element);

} // namespace ImageOps

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
