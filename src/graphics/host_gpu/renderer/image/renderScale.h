#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_RENDERSCALE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_RENDERSCALE_H_

// Internal render scale (--render-scale): the host allocates frame-sized render targets and
// depth buffers at a fraction of their guest size. The guest never sees the change: ImageInfo
// keeps the guest geometry (memory layout, uploads, downloads), only the Vulkan backing shrinks.
// Draws into a scaled target scale their viewport and scissor; pixel and compute shaders scale
// gl_FragCoord, texel fetches/stores and size queries of scaled images from the push data
// (ShaderIR PushData::Rescale*). Anything that cannot be scaled (guest-memory transfers, mixed
// passes, texel access from other stages) blits the image back to guest size first.
//
// The helpers here are pure so tests cover the geometry rules.

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Libs::Graphics::RenderScale {

// Scales within this distance of 1.0 are treated as "off".
inline constexpr float IDENTITY_EPSILON = 1e-3f;

// Only images "at or near the frame size" are scaled: at least a 1024x576 2D surface with a
// screen-like aspect ratio. Square shadow maps, atlases and small effect buffers stay 1:1.
inline constexpr uint32_t MIN_WIDTH  = 1024;
inline constexpr uint32_t MIN_HEIGHT = 576;
inline constexpr float    MIN_ASPECT = 1.25f;
inline constexpr float    MAX_ASPECT = 2.5f;

struct Rect {
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;

	bool operator==(const Rect&) const = default;
};

[[nodiscard]] constexpr bool Active(float scale) noexcept {
	return scale > 0.0f && std::fabs(scale - 1.0f) > IDENTITY_EPSILON;
}

// Nearest scaled size, never below one texel.
[[nodiscard]] constexpr uint32_t ScaleDimension(uint32_t value, float scale) noexcept {
	const float scaled = static_cast<float>(value) * scale + 0.5f;
	return std::max(1u, static_cast<uint32_t>(scaled));
}

[[nodiscard]] constexpr vk::Extent3D ScaleExtent(vk::Extent3D extent, float scale) noexcept {
	return {ScaleDimension(extent.width, scale), ScaleDimension(extent.height, scale),
	        extent.depth};
}

[[nodiscard]] constexpr bool FrameSized(uint32_t width, uint32_t height) noexcept {
	if (width < MIN_WIDTH || height < MIN_HEIGHT) {
		return false;
	}
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	return aspect >= MIN_ASPECT && aspect <= MAX_ASPECT;
}

// Geometry a scaled backing can represent: a single-sample, single-mip 2D (array) surface of
// uncompressed texels at frame size. Mip chains, volumes and MSAA keep their guest size.
[[nodiscard]] constexpr bool GeometryEligible(Prospero::ImageType type, vk::Extent3D extent,
                                              uint32_t levels, uint32_t samples,
                                              bool block_compressed) noexcept {
	return type == Prospero::ImageType::kColor2D && extent.depth == 1 && levels == 1 &&
	       samples == 1 && !block_compressed && FrameSized(extent.width, extent.height);
}

// Guest scissor to host pixels: the rectangle grows outward to whole host pixels and is
// clamped to the host extent. An empty guest rectangle stays empty.
[[nodiscard]] constexpr Rect ScaleRect(Rect rect, float scale, uint32_t host_width,
                                       uint32_t host_height) noexcept {
	if (rect.right <= rect.left || rect.bottom <= rect.top) {
		return {0, 0, 0, 0};
	}
	const auto floor_scaled = [scale](int value) {
		return static_cast<int>(std::floor(static_cast<float>(value) * scale));
	};
	const auto ceil_scaled = [scale](int value) {
		return static_cast<int>(std::ceil(static_cast<float>(value) * scale));
	};
	const int width  = static_cast<int>(host_width);
	const int height = static_cast<int>(host_height);
	Rect      scaled {std::clamp(floor_scaled(rect.left), 0, width),
	             std::clamp(floor_scaled(rect.top), 0, height),
	             std::clamp(ceil_scaled(rect.right), 0, width),
	             std::clamp(ceil_scaled(rect.bottom), 0, height)};
	if (scaled.right <= scaled.left || scaled.bottom <= scaled.top) {
		return {0, 0, 0, 0};
	}
	return scaled;
}

// Guest viewport to host pixels; the depth range is unchanged.
[[nodiscard]] constexpr vk::Viewport ScaleViewport(vk::Viewport viewport, float scale) noexcept {
	viewport.x *= scale;
	viewport.y *= scale;
	viewport.width *= scale;
	viewport.height *= scale;
	return viewport;
}

} // namespace Libs::Graphics::RenderScale

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_RENDERSCALE_H_
