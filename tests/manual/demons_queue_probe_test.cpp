#include "graphics/host_gpu/renderer/demonsQueueProbe.h"

#include <cassert>
#include <fstream>
#include <vector>

using namespace Libs::Graphics::DemonsQueueProbe;

static std::vector<uint32_t> ReadWords(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	const auto size = file.tellg();
	assert(size > 0 && size % sizeof(uint32_t) == 0);
	std::vector<uint32_t> words(size / sizeof(uint32_t));
	file.seekg(0);
	assert(file.read(reinterpret_cast<char*>(words.data()), size));
	return words;
}

int main(int argc, char** argv) {
	static_assert(DefaultPixels(1600, 900)[4] == std::pair{1200u, 675u});
	constexpr auto coverage = MaterialCoveragePixels(1600, 900, 0);
	static_assert(coverage.size() == 96 && coverage.front() == std::pair{33u, 28u});
	static_assert(coverage.back() == std::pair{1500u, 815u});
	static_assert(MaterialCoveragePixels(1600, 900, 8).back() == std::pair{1566u, 871u});
	static_assert(PixelInScissor({12, 9}, 10, 9, 3, 1));
	static_assert(!PixelInScissor({13, 9}, 10, 9, 3, 1));
	static_assert(!PixelInScissor({9, 9}, 10, 9, 3, 1));
	static_assert(SampleBlock(5, 12).offset == 4 && SampleBlock(5, 12).extent == 4);
	static_assert(SampleBlock(8, 10).offset == 8 && SampleBlock(8, 10).extent == 2);
	static_assert(SampleBlock(0, 1).offset == 0 && SampleBlock(0, 1).extent == 1);
	static_assert(PackedRgb10IsBlack(0xc0000000u));
	static_assert(!PackedRgb10IsBlack(0xc0000001u));
	static_assert(!PackedRgb10IsBlack(0x00100000u));
	static_assert(MaterialTracePending(891, 892));
	static_assert(MaterialTracePending(891, 903));
	static_assert(!MaterialTracePending(891, 904) && !MaterialTracePending(891, 890));
	std::vector<uint32_t> albedo(17 * 9, 0xff000000u);
	albedo[3 * 17 + 8] = 0xff112233u;
	albedo[8 * 17 + 16] = 0xff223344u;
	constexpr std::array<uint32_t, 4> colored_tiles {1, 5, 1, 100};
	Pixels covered_albedo {};
	assert(ColoredTilePixels(albedo, colored_tiles, 17, 9, covered_albedo) == 2);
	assert((covered_albedo[0] == std::pair{8u, 3u}));
	assert((covered_albedo[1] == std::pair{16u, 8u}));
	assert(ColoredTilePixels(albedo, std::array<uint32_t, 1>{0}, 17, 9, covered_albedo) == 0);
	static_assert(PostTracePending(961, 963) && !PostTracePending(961, 964));
	static_assert(!PostTracePending(-1, 0));
	Capture scene {961, Shaders[1], 1600, 900};
	assert(scene.TracesImage(HdrAddress) && scene.TracesImage(SceneAddress));
	assert(!scene.TracesImage(0) && !scene.TracesImage(0x123000));
	scene.TrackIntermediate(0x123000, 799, 900);
	assert(!scene.TracesImage(0x123000));
	scene.TrackIntermediate(0x123000, 1920, 1080);
	assert(scene.TracesImage(0x123000));
	std::vector<uint32_t> prefix(65);
	std::vector<uint32_t> tiles(45200);
	prefix[1] = 22089;
	prefix[33] = 22600;
	prefix[34] = 45200;
	tiles[0] = 8;
	tiles[22088] = 22599;
	tiles[45199] = 22599;

	if (argc == 3) {
		prefix = ReadWords(argv[1]);
		tiles = ReadWords(argv[2]);
	}
	const auto queue0 = SelectTiles(prefix, tiles, 0);
	const auto queue33 = SelectTiles(prefix, tiles, 33);
	assert(queue0.valid && queue0.first == 0 && queue0.end == 22089);
	assert(queue33.valid && queue33.first == 22600 && queue33.end == 45200);
	Pixels pixels {};
	assert(TilePixels(queue0, 1600, 900, pixels));
	assert((pixels.front() == std::pair {68u, 4u}));
	assert((pixels.back() == std::pair {1596u, 899u}));
	assert(TilePixels(queue33, 1600, 900, pixels));
	assert((pixels.front() == std::pair {4u, 4u}));
	assert((pixels.back() == std::pair {1596u, 899u}));
	if (argc == 3) {
		assert((queue0.ids == std::array {8u, 6858u, 12280u, 15556u, 22599u}));
		assert((queue33.ids == std::array {0u, 6809u, 10443u, 18925u, 22599u}));
	}
	assert(!TilePixels(queue33, 1600, 899 - 4, pixels));
	assert(!SelectTiles(prefix, std::span(tiles).first(45199), 33).valid);
	assert(!SelectTiles(prefix, tiles, UINT32_MAX).valid);
	assert(!SelectTiles(prefix, tiles, 2).valid);
}
