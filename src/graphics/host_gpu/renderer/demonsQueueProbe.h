#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace Libs::Graphics::DemonsQueueProbe {

constexpr uint64_t HdrAddress = 0x281300000ull;
constexpr uint64_t TileProducer = 0x32c7da582db1795aull;
constexpr uint64_t PrefixAddress = 0x208e41a00ull;
constexpr uint64_t TilesAddress = 0x208cc5ee0ull;
constexpr std::array<uint32_t, 2> Queues {0, 33};
constexpr std::array<uint64_t, 2> Shaders {0xf0c482b0b355e9deull, 0xe3daeb1e7426761bull};
using Pixels = std::array<std::pair<uint32_t, uint32_t>, 5>;

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
