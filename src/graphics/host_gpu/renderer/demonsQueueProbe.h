#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace Libs::Graphics::DemonsQueueProbe {

constexpr uint64_t GBufferAddress = 0x287ae0000ull;
// Observed with that array bound as a color attachment in the gameplay trace.
constexpr std::array<uint64_t, 3> GBufferShaders {
    0xafd9334e8581cda2ull, 0xe31a5473e9ee7503ull, 0xb631777eb4aee68cull
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

struct MaterialTrace {
	int32_t frame = -1;
	uint64_t shader = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	Pixels pixels {};
	uint32_t count = 0;
	uint32_t lighting_sampled = 0;
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
};

} // namespace Libs::Graphics::DemonsQueueProbe
