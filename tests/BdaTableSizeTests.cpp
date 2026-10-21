// KYTY_BDA_TABLE sizing rules (graphics/host_gpu/renderer/cache/bdaTableSize.h).
#include "graphics/host_gpu/renderer/cache/bdaTableSize.h"

#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;

static void Check(bool c, const char* t) {
	if (!c) {
		std::fprintf(stderr, "BdaTableSizeTests: %s\n", t);
		std::abort();
	}
}

int main() {
	constexpr uint64_t g     = BdaTableSize::kGranulePages;
	constexpr uint64_t dense = uint64_t {448} << 16u; // 448 GiB of 16 KiB pages
	Check(BdaTableSize::PagesFor(0, dense) == g, "an empty table is one granule");
	Check(BdaTableSize::PagesFor(g, dense) == g, "a full granule fits exactly");
	Check(BdaTableSize::PagesFor(g + 1, dense) == 2 * g, "one page past a granule takes the next");
	Check(BdaTableSize::PagesFor(dense + 5, dense) == dense, "never past the dense size");
	// Astro Bot's mapped high-water 0x11ea000000 (71.7 GiB) -> 9 granules = 36 MiB.
	constexpr uint64_t astro = (uint64_t {0x11ea000000} >> 14u);
	Check(BdaTableSize::PagesFor(astro, dense) * 8u == uint64_t {36} << 20u, "Astro high-water -> 36 MiB");
	Check(BdaTableSize::GrowTo(2 * g, g, dense) == 2 * g, "a fitting request does not shrink");
	Check(BdaTableSize::GrowTo(2 * g, 2 * g, dense) == 2 * g, "an exact fit does not grow");
	Check(BdaTableSize::GrowTo(2 * g, 2 * g + 1, dense) == 3 * g, "a larger request grows by granules");
	Check(BdaTableSize::GrowTo(dense, dense, dense) == dense, "the dense table never grows");
	Check(BdaTableSize::Sized(nullptr, true) && BdaTableSize::Sized("", true), "sized by default");
	Check(!BdaTableSize::Sized("dense", true), "KYTY_BDA_TABLE=dense opts out");
	Check(BdaTableSize::Sized("densely", true), "only the exact value opts out");
	Check(!BdaTableSize::Sized(nullptr, false), "dense without zeroed out-of-range reads");
	std::puts("BdaTableSizeTests: ok");
	return 0;
}
