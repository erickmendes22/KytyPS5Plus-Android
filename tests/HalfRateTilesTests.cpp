#include "graphics/shader/halfRateTiles.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace H = Libs::Graphics::HalfRateTiles;

// 11 arrays of 64 words: valid, depth, 4 G-buffer, 4 color, luminance.
static_assert(H::kLdsDwords == 704);
// One 8x8 wave per tile; the 256 culling items take 4 passes of 64 threads.
static_assert(H::kThreads == 64 && H::kCullPasses == 4 && H::kThreads * H::kCullPasses == H::kGuestThreads);
// The kernel's 1920 LDS words plus the patch stay well under the 32 KiB compute LDS of Adreno.
static_assert((1920u + H::kLdsDwords) * 4u <= 32u * 1024u);
// Spare VGPRs are above the kernel's v0..v71 and inside the 256 the IR has.
static_assert(H::kRegOriginX > 71 && H::kRegTileLive < 256);
// A 1920x1080 screen of 16x16 tiles fits the check log's tile grid.
static_assert(H::kCheckTilesX * 16 >= 1920 && H::kCheckTilesY * 16 >= 1080);
// Entry: real list, item count, stress designed count, start, stress list (256), mask (8).
static_assert(H::kCheckEntryDwords == 294 && H::kCheckMaskDwords == 8);
// The stress scratch fits next to the patch in the 32 KiB compute LDS.
static_assert((1920u + H::kLdsDwords + H::kCheckLdsDwords) * 4u <= 32u * 1024u);

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HalfRateTilesTests: failed: %s\n", message);
		g_failures++;
	}
}

// One capture: real list (count + 1, entries), item count 1, and a stress list that holds the
// expected items of the tile in ascending order (what a correct culling appends) unless `stress`
// is given.
void Put(std::vector<uint32_t>& region, uint32_t tile, std::vector<uint32_t> entries, uint32_t count = UINT32_MAX,
         const std::vector<uint32_t>* stress = nullptr) {
	auto* e = region.data() + tile * H::kCheckEntryDwords;
	e[0]    = (count == UINT32_MAX ? static_cast<uint32_t>(entries.size()) : count) + 1u;
	for (size_t i = 0; i < entries.size() && i < H::kListEntries; i++) {
		e[1 + i] = entries[i];
	}
	e[H::kCheckWordItems]    = 1;
	e[H::kCheckWordDesigned] = H::StressDesignedCount(tile);
	e[H::kCheckWordStart]    = 1;
	std::vector<uint32_t> list;
	if (stress != nullptr) {
		list = *stress;
	} else {
		for (uint32_t k = 0; k < H::StressDesignedCount(tile); k++) {
			if (k < 1u || H::StressPasses(k, tile)) list.push_back(k);
		}
	}
	e[H::kCheckWordStressN] = static_cast<uint32_t>(list.size());
	for (size_t i = 0; i < list.size(); i++) {
		e[H::kCheckWordStress + i] = list[i];
		e[H::kCheckWordMask + list[i] / 32u] |= 1u << (list[i] % 32u);
	}
}

void TestCompare() {
	std::vector<uint32_t> native(H::kCheckRegionDwords, 0u);
	std::vector<uint32_t> half(H::kCheckRegionDwords, 0u);
	Put(native, 0, {3, 7, 9});
	Put(half, 0, {3, 7, 9}); // same order
	Put(native, 1, {3, 7, 9});
	Put(half, 1, {9, 3, 7}); // same set, other append order
	Put(native, 2, {3, 7, 9});
	Put(half, 2, {3, 7, 8}); // different item
	Put(native, 3, {3, 7});
	Put(half, 3, {3, 7, 9}); // different count
	Put(native, 4, {}, 20);
	Put(half, 4, {}, 20);    // more than 16 appended: entries past 16 are not kept
	Put(native, 5, {});
	Put(half, 5, {});        // empty list, still captured
	Put(native, 6, {1});     // half never ran on this tile: skipped
	Put(native, 7, {5, 6});
	Put(half, 7, {5, 6});
	half[7 * H::kCheckEntryDwords + 5] = 99; // stale word past the count does not matter
	// Tile 3's designed stress count is 64 (a pass boundary): half appends in another order.
	std::vector<uint32_t> reversed;
	for (uint32_t k = H::StressDesignedCount(3); k-- > 0;) {
		if (k < 1u || H::StressPasses(k, 3)) reversed.push_back(k);
	}
	Put(half, 3, {3, 7, 9}, UINT32_MAX, &reversed);
	// Tile 8 (designed 191): half loses one item.
	Put(native, 8, {2});
	std::vector<uint32_t> missing;
	for (uint32_t k = 0; k < H::StressDesignedCount(8); k++) {
		if (k < 1u || H::StressPasses(k, 8)) missing.push_back(k);
	}
	missing.pop_back();
	Put(half, 8, {2}, UINT32_MAX, &missing);

	const auto c = H::CompareLists(native.data(), half.data(), H::kCheckTiles);
	Check(c.tiles == 8, "tiles captured in both");
	Check(c.same_order == 4, "real lists same order: tiles 0, 5, 7, 8");
	Check(c.same_set == 1, "real list same set in another order: tile 1");
	Check(c.different == 2, "real lists different: tiles 2 and 3");
	Check(c.overflowed == 1, "real list overflow: tile 4");
	Check(c.first_diff == 2, "first real-list difference");
	Check(c.stress_same_order == 6 && c.stress_same_set == 1 && c.stress_different == 1,
	      "stress lists: tile 3 reordered, tile 8 missing an item");
	Check(c.stress_expected == 7, "stress lists as expected in both except tile 8");
	Check(c.mask_equal == 7 && c.mask_diff == 1, "stress masks differ only on tile 8");
	Check(c.stress_boundary == 1, "one compared tile with a boundary count (tile 3: 64)");
	Check(H::StressDesignedCount(3) == 64 && H::StressDesignedCount(6) == 128 && H::StressDesignedCount(9) == 192,
	      "boundary counts appear in the cycle");
}

void TestVariant() {
	unsetenv("KYTY_RT_HALF");
	Check(H::Variant(H::kShaderHash, 2, 16, 16, false) == H::kVariantHalf, "live rt_half=1 selects the half variant");
	Check(H::Variant(H::kShaderHash, 1, 16, 16, false) == 0, "live rt_half=0 keeps the kernel");
	Check(H::Variant(H::kShaderHash, 1, 16, 16, true) == H::kVariantCaptureOnly, "check log captures the native kernel");
	Check(H::Variant(H::kShaderHash, 0, 16, 16, false) == 0, "env unset keeps the patch off");
	Check(H::Variant(H::kShaderHash, 2, 8, 8, false) == 0, "unexpected group size is not patched");
	Check(H::Variant(0xd2c5a6652fd00b38ull, 2, 16, 16, true) == 0, "sibling tile-class kernel is not patched");
	Check(H::Variant(H::kShaderHash ^ 1u, 2, 16, 16, true) == 0, "neighbouring hash is not patched");
}

} // namespace

int main() {
	TestCompare();
	TestVariant();
	if (g_failures != 0) return 1;
	std::printf("HalfRateTilesTests: ok\n");
	return 0;
}
