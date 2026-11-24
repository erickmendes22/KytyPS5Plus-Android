#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace Libs::Graphics::DemonsQueueProbe {

constexpr uint64_t GBufferAddress = 0x287ae0000ull;
// Observed with that array bound as a color attachment in the gameplay trace.
// The final two hashes are the two largest still-unmeasured opaque
// material draws in the 765 dark-world frame (35,964 and 17,760 indices).
// Capture only their first post-transition draw to test whether they survive
// depth/stencil and write the scenery's deferred material; no draw is altered.
constexpr std::array<uint64_t, 6> GBufferShaders {
    0xafd9334e8581cda2ull, 0xe31a5473e9ee7503ull, 0xb631777eb4aee68cull,
    0xcfc1d9193e7f9e53ull, 0xc92deb23fc611b10ull, 0x552d823dcfcf2deeull
};

struct BlockAxis {
	uint32_t offset;
	uint32_t extent;
};

// Vulkan compressed copies start on a block boundary and may end on the mip edge.
constexpr BlockAxis SampleBlock(uint64_t texel, uint32_t mip_extent) {
	const auto offset = static_cast<uint32_t>(texel) & ~3u;
	return {offset, std::min(4u, mip_extent - offset)};
}

constexpr uint64_t HdrAddress = 0x281300000ull;
constexpr uint64_t SceneAddress = 0x281a80000ull;
constexpr uint64_t CompositeAuxAddress = 0x2b2a40000ull;
constexpr uint64_t CompositeShader = 0x3265b01ed00a5c01ull;
constexpr std::array<uint64_t, 6> PostShaders {
    CompositeShader, 0x17defa333d9d1e22ull, 0xd1768d02fcf96a58ull,
    0x844e5697cf62ecd7ull, 0xa5fc1658592e3cbaull, 0x41a57a218d07a778ull
};

constexpr bool PostImage(uint64_t address) {
	return address == HdrAddress || address == SceneAddress || address == CompositeAuxAddress;
}

constexpr bool PostTracePending(int32_t start, int32_t current) {
	const int64_t age = int64_t{current} - start;
	return start >= 0 && age >= 0 && age <= 2;
}
// The scene transfer and composite may occur after the bloom mip chain.
constexpr bool TracePostPass(uint32_t pass_count, uint32_t scene_count,
                             bool writes_scene, bool final_composite) {
	return final_composite || (writes_scene ? scene_count < 8u : pass_count < 24u);
}
constexpr uint64_t TileProducer = 0x32c7da582db1795aull;
constexpr uint64_t PrefixAddress = 0x208e41a00ull;
constexpr uint64_t TilesAddress = 0x208cc5ee0ull;
constexpr std::array<uint32_t, 2> Queues {0, 33};
constexpr std::array<uint64_t, 2> Shaders {0xf0c482b0b355e9deull, 0xe3daeb1e7426761bull};
using Pixels = std::array<std::pair<uint32_t, uint32_t>, 5>;

constexpr Pixels DefaultPixels(uint32_t width, uint32_t height) {
	return {{{width / 2, height / 2}, {width / 4, height / 4},
	         {3 * width / 4, height / 4}, {width / 4, 3 * height / 4},
	         {3 * width / 4, 3 * height / 4}}};
}

constexpr bool PixelInScissor(std::pair<uint32_t, uint32_t> pixel, int32_t x, int32_t y,
                              uint32_t width, uint32_t height) {
	const auto dx = int64_t{pixel.first} - x;
	const auto dy = int64_t{pixel.second} - y;
	return dx >= 0 && dy >= 0 && dx < width && dy < height;
}

// Centers of a regular 12-by-8 grid; vary the offset within each cell
// between captures to avoid repeatedly hitting gaps between small meshes.
constexpr uint32_t CoverageColumns = 12;
constexpr uint32_t CoverageRows = 8;
using CoveragePixels =
    std::array<Pixels::value_type, CoverageColumns * CoverageRows>;

constexpr CoveragePixels MaterialCoveragePixels(uint32_t width, uint32_t height,
                                                uint32_t phase) {
	CoveragePixels pixels {};
	const auto x_offset = 1u + (phase % 3u);
	const auto y_offset = 1u + (phase / 3u % 3u);
	for (uint32_t i = 0; i < pixels.size(); ++i) {
		pixels[i] = {((i % CoverageColumns) * 4u + x_offset) * width / (4u * CoverageColumns),
		             ((i / CoverageColumns) * 4u + y_offset) * height / (4u * CoverageRows)};
	}
	return pixels;
}

// GetFrameNum counts guest Done calls, which can advance while the queued
// GPU work is still being recorded. Age bounds the diagnostic only.
constexpr bool MaterialTracePending(int32_t producer, int32_t consumer) {
	const int64_t age = int64_t{consumer} - producer;
	return producer >= 0 && age >= 0 && age <= 12;
}

struct MaterialTrace {
	int32_t frame = -1;
	uint64_t shader = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	Pixels pixels {};
	uint32_t count = 0;
	uint32_t lighting_sampled = 0;
	uint32_t lighting_in_progress = 0;
	uint64_t submit = 0;
	uint32_t image_index = UINT32_MAX;
	uint32_t image_generation = 0;
	uint64_t content_version = 0;
	std::array<uint32_t, Pixels{}.size()> output {};
};

struct Tiles {
	uint32_t first = 0;
	uint32_t end = 0;
	std::array<uint32_t, 5> ids {};
	bool valid = false;
};

inline Tiles SelectTiles(std::span<const uint32_t> offsets, std::span<const uint32_t> tiles,
                         uint32_t queue) {
	if (uint64_t{queue} + 1 >= offsets.size()) return {};
	Tiles result {offsets[queue], offsets[queue + 1]};
	if (result.first >= result.end || result.end > tiles.size()) return result;
	for (size_t i = 0; i < result.ids.size(); ++i) {
		const auto index = result.first +
		    uint64_t{result.end - result.first - 1} * i / (result.ids.size() - 1);
		result.ids[i] = tiles[index];
	}
	result.valid = true;
	return result;
}

inline bool TilePixels(const Tiles& tiles, uint32_t width, uint32_t height, Pixels& output) {
	if (!tiles.valid || width == 0 || height == 0) return false;
	const auto pitch = (uint64_t{width} + 7u) / 8u;
	Pixels pixels {};
	for (size_t i = 0; i < pixels.size(); ++i) {
		const auto x = tiles.ids[i] % pitch * 8u;
		const auto y = tiles.ids[i] / pitch * 8u;
		if (x >= width || y >= height) return false;
		pixels[i] = {std::min(static_cast<uint32_t>(x) + 4u, width - 1),
		             std::min(static_cast<uint32_t>(y) + 4u, height - 1)};
	}
	output = pixels;
	return true;
}

// Select actual nonblack albedo texels belonging to an executed queue.
inline uint32_t ColoredTilePixels(std::span<const uint32_t> albedo,
                                  std::span<const uint32_t> tiles, uint32_t width,
                                  uint32_t height, Pixels& output) {
	if (width == 0 || height == 0 || albedo.size() != uint64_t{width} * height) return 0;
	const uint64_t pitch = (uint64_t{width} + 7) / 8;
	Pixels found {};
	uint32_t count = 0;
	const size_t stride = std::max<size_t>(tiles.size() / 17, 1);
	for (size_t phase = 0; phase < stride && count < found.size(); ++phase) {
		for (size_t i = phase; i < tiles.size() && count < found.size(); i += stride) {
			const uint64_t left = (tiles[i] % pitch) * 8;
			const uint64_t top = (tiles[i] / pitch) * 8;
			if (left >= width || top >= height) continue;
			bool picked = false;
			for (uint64_t y = top; !picked && y < std::min<uint64_t>(top + 8, height); ++y) {
				for (uint64_t x = left; x < std::min<uint64_t>(left + 8, width); ++x) {
					if (!(albedo[y * width + x] & 0x00ffffffu)) continue;
					const Pixels::value_type point {x, y};
					if (std::ranges::find(found.begin(), found.begin() + count, point) ==
					    found.begin() + count) {
						found[count++] = point;
						picked = true;
						break;
					}
				}
			}
		}
	}
	if (count) {
		std::fill(found.begin() + count, found.end(), found[0]);
		output = found;
	}
	return count;
}

// A2B10G10R10 and A2R10G10B10 put opacity in the most significant two bits.
constexpr bool PackedRgb10IsBlack(uint32_t word) {
	return (word & 0x3fffffffu) == 0;
}

struct Capture {
	int32_t frame = -1;
	uint64_t shader = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	Pixels pixels {};
	bool hdr_reader_sampled = false;
	bool hdr_writer_sampled = false;
	bool composite_sampled = false;
	bool light_output_sampled = false;
	uint32_t post_passes = 0;
	uint32_t scene_passes = 0;
	std::array<uint64_t, 12> post_intermediates {};

	bool TracesImage(uint64_t address) const {
		return PostImage(address) || (address != 0 &&
		    std::ranges::find(post_intermediates, address) != post_intermediates.end());
	}

	void TrackIntermediate(uint64_t address, uint32_t w, uint32_t h) {
		if (address == 0 || w < width / 2 || h < height / 2 || TracesImage(address)) return;
		const auto slot = std::ranges::find(post_intermediates, uint64_t{0});
		if (slot != post_intermediates.end()) *slot = address;
	}
};

} // namespace Libs::Graphics::DemonsQueueProbe
