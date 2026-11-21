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
