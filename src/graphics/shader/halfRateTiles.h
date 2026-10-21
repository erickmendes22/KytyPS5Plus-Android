#ifndef GRAPHICS_SHADER_HALF_RATE_TILES_H
#define GRAPHICS_SHADER_HALF_RATE_TILES_H

// KYTY_RT_HALF=1 / KYTY_LIVE_KNOBS rt_half=0|1 (experiment, off by default): Astro Bot's tiled
// deferred-lighting kernel CS 6856320a (research/NEXT.md "c17_rtC") shades one pixel in four.
// The game's resolution and every other pass are unchanged.
//
// The kernel runs one 16x16 workgroup per screen tile from a GPU-built tile list. With the patch
// the group is 8x8 (one wave), and each thread shades the even/even pixel of its 2x2. That frees
// three of the four wave slots a tile used to hold (at 48 registers an SP fits 4 waves; a 16x16
// group with idle waves saved nothing, research/NEXT.md "c17_rtH").
//
// The prologue culls the light list cooperatively, one item per thread over 256 threads: thread 0
// writes the tile's planes and clears the counter, then a barrier, then each thread tests item
// v23 and appends passing items with an LDS atomic. With 64 threads, the item code
// (guest pcs 0x374..0x4d4, straight-line apart from EXECZ skips) is replayed 4 times per thread
// with v23 = thread + 64k. Every item is tested by the same instructions and appended through
// the same atomic. The set of appended items is the same; only the append order can differ,
// and it differs natively between waves too. The consumer starts at a rotating offset and sums
// per-light terms (research/NEXT.md). The original code then runs with EXEC off.
//
// Wave 0 also saves its samples (depth, G-buffer value, lit color, luminance) to LDS words added
// after the guest's. Before s_endpgm a barrier, then each thread fills the 3 other pixels of its
// 2x2 from the up-to-4 nearest samples in the tile: a joint bilateral upsample whose depth and
// G-buffer terms reject other surfaces, so an edge pixel takes the nearest same-surface sample.
// The results go through the guest's own output descriptors.
//
// KYTY_RT_HALF_CHECK=1 (diagnostic): both variants write each tile's light list (count + 16
// entries, after the second barrier) to a host-visible log, native and half in separate regions.
// The host compares them as sets per tile ("[rt-half-check]", BufferCache::PollRtHalfCheck).
//
// Only the listed hash with the expected instructions at the patch points is changed. Other
// shaders, or a mismatch, compile as before (the recompiler logs the reason).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::HalfRateTiles {

inline constexpr uint64_t kShaderHash = 0x6856320a9c6f89fdull;

inline constexpr uint32_t kGuestGroupSize = 16; // guest threads per axis
inline constexpr uint32_t kGroupSize      = 8;  // patched threads per axis
inline constexpr uint32_t kThreads        = kGroupSize * kGroupSize;
inline constexpr uint32_t kGuestThreads   = kGuestGroupSize * kGuestGroupSize;
inline constexpr uint32_t kCullPasses     = kGuestThreads / kThreads;

// LDS words appended after the guest's: 11 arrays of 64 words (valid flag, depth, 4 G-buffer
// words, 4 color words, luminance).
inline constexpr uint32_t kSamples   = kThreads;
inline constexpr uint32_t kArrays    = 11;
inline constexpr uint32_t kLdsDwords = kSamples * kArrays;

// Guest pcs of 6856320a used by the patch.
inline constexpr uint32_t kPcFlatIndex   = 0x038;  // V_LSHL_ADD_U32 v23, v1, 4, v0
inline constexpr uint32_t kPcCullBegin   = 0x374;  // first item-test instruction after barrier 1
inline constexpr uint32_t kPcCullEnd     = 0x4d4;  // S_MOV_B64 exec, -1 after the append
inline constexpr uint32_t kPcCoordX      = 0x4d8;  // V_ADD_NC_U32 v24, s12, v0
inline constexpr uint32_t kPcCoordY      = 0x4dc;  // V_ADD_NC_U32 v25, s13, v1
inline constexpr uint32_t kPcListBarrier = 0x4e4;  // s_barrier after the light list is complete
inline constexpr uint32_t kPcBoundsCmp   = 0x500;  // V_CMP_LE_U32 s10, s13, v25 (s12/s13 = extent)
inline constexpr uint32_t kPcDepthDesc   = 0x51c;  // S_LOAD_DWORDX8 s16, s14 + 128
inline constexpr uint32_t kPcGbufferDesc = 0x524;  // S_LOAD_DWORDX8 s24, s14 + 0
inline constexpr uint32_t kPcDepthLoad   = 0x534;  // IMAGE_LOAD v26, v24, s16 (R32 depth)
inline constexpr uint32_t kPcGbufferLoad = 0x53c;  // IMAGE_LOAD v3..v6, v24, s24
inline constexpr uint32_t kPcColorDesc   = 0x41bc; // S_LOAD_DWORDX8 s0, s14 + 448
inline constexpr uint32_t kPcColorStore  = 0x4228; // IMAGE_STORE v3..v6, v24, s0 (RGBA16F)
inline constexpr uint32_t kPcLumaDesc    = 0x425c; // S_LOAD_DWORDX8 s0, s14 + 480
inline constexpr uint32_t kPcLumaStore   = 0x4268; // IMAGE_STORE v8, v24, s0 (R16F)
inline constexpr uint32_t kPcEnd         = 0x4270; // s_endpgm

// Light list in the guest's LDS: counter at byte 128, entries at byte 64 (at most 16).
inline constexpr uint32_t kListCounterByte = 128;
inline constexpr uint32_t kListEntriesByte = 64;
inline constexpr uint32_t kListEntries     = 16;

// Spare VGPRs (the kernel uses v0..v71) carrying values from the prologue to the end.
inline constexpr uint32_t kRegOriginX  = 240;
inline constexpr uint32_t kRegOriginY  = 241;
inline constexpr uint32_t kRegExtentX  = 242;
inline constexpr uint32_t kRegExtentY  = 243;
inline constexpr uint32_t kRegTileLive = 244; // 1 once the tile passed the guest's empty-tile exit

// Fill of the 3 unshaded pixels per 2x2 (ShaderComputeInputInfo::rt_fill_live, live knob rt_fill):
// rt_fill=4 (default) the up-to-4 nearest samples, weights without a floor so that when all are
// rejected the least different one wins; rt_fill=9 also the ring around them (always computed:
// one IR block has no branch to skip it); rt_fill=0 no fill (timing arm, image stale);
// rt_fill=99 each thread shades a contiguous pixel of the tile's top-left 8x8 quarter instead of
// a 2x-strided one, no fill (timing probe for lane coherence, image wrong).
inline constexpr uint8_t kFillNearest4 = 0;
inline constexpr uint8_t kFillOff      = 1;
inline constexpr uint8_t kFillRing9    = 2;
inline constexpr uint8_t kFillQuarter  = 3;

inline uint8_t FillModeFromKnob(int64_t value) {
	switch (value) {
		case 0: return kFillOff;
		case 9: return kFillRing9;
		case 99: return kFillQuarter;
		default: return kFillNearest4;
	}
}

// program variant (ShaderComputeInputInfo::half_rate_tiles)
inline constexpr uint8_t kVariantHalf        = 1;
inline constexpr uint8_t kVariantCaptureOnly = 2; // native kernel, light-list capture only

// Check log: per region kCheckTiles entries of kCheckEntryDwords:
//   [0] light-list count + 1 (0 = no capture for that tile), [1..16] list entries,
//   [17] the item count the guest culls (s51), [18..25] a 256-bit mask of the items that pass the
//   guest's item test when kForcedItems items are tested (the "stress" culling: the same item code
//   over items past the real count, read from the light buffer beyond its live entries, without the
//   16-entry append cap). Native runs it with 256 threads, one item each; half with 64 threads x 4.
inline constexpr uint32_t kForcedItems       = 256;
inline constexpr uint32_t kCheckMaskDwords   = kForcedItems / 32u;
inline constexpr uint32_t kCheckWordCount    = 1u + kListEntries;
inline constexpr uint32_t kCheckWordItems    = kCheckWordCount;
inline constexpr uint32_t kCheckWordMask     = kCheckWordItems + 1u;
inline constexpr uint32_t kCheckTilesX       = 128;
inline constexpr uint32_t kCheckTilesY       = 72;
inline constexpr uint32_t kCheckTiles        = kCheckTilesX * kCheckTilesY;
inline constexpr uint32_t kCheckEntryDwords  = kCheckWordMask + kCheckMaskDwords;
inline constexpr uint32_t kCheckRegionDwords = kCheckTiles * kCheckEntryDwords;
inline constexpr uint32_t kCheckBytes        = 2u * kCheckRegionDwords * 4u;
inline constexpr uint32_t kCheckRegionNative = 0;
inline constexpr uint32_t kCheckRegionHalf   = 1;

inline bool EnvFlag(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

inline bool EnvEnabled() {
	static const bool enabled = EnvFlag("KYTY_RT_HALF");
	return enabled;
}

inline bool CheckEnabled() {
	static const bool enabled = EnvFlag("KYTY_RT_HALF_CHECK");
	return enabled;
}

// Device address of the check log (set by the buffer cache before shaders compile), 0 = none.
inline std::atomic<uint64_t>& CheckAddressStorage() {
	static std::atomic<uint64_t> address {0};
	return address;
}
inline uint64_t CheckAddress() { return CheckAddressStorage().load(std::memory_order_acquire); }
inline void     SetCheckAddress(uint64_t address) { CheckAddressStorage().store(address, std::memory_order_release); }

// live: 0 = KYTY_RT_HALF decides, 1 = off, 2 = on.
inline bool Applies(uint64_t hash, uint8_t live) {
	if (hash != kShaderHash) {
		return false;
	}
	return live == 0 ? EnvEnabled() : live == 2;
}

// The variant a program of this hash compiles as: half, capture-only (check log on), or none.
inline uint8_t Variant(uint64_t hash, uint8_t live, uint32_t threads_x, uint32_t threads_y, bool check) {
	if (hash != kShaderHash || threads_x != kGuestGroupSize || threads_y != kGuestGroupSize) {
		return 0;
	}
	if (Applies(hash, live)) {
		return kVariantHalf;
	}
	return check ? kVariantCaptureOnly : uint8_t {0};
}

// Light-list comparison of two check-log regions (host side). A tile counts when both regions
// captured it (first word = count + 1, non-zero).
struct ListCompare {
	uint32_t tiles       = 0; // present in both
	uint32_t same_order  = 0; // identical count and entries in the same order
	uint32_t same_set    = 0; // identical count and the same entries in another order
	uint32_t different   = 0;
	uint32_t overflowed  = 0; // count > 16 in either (entries past 16 are not kept, set undefined)
	int32_t  first_diff  = -1;
	uint32_t mask_equal  = 0; // stress culling: identical 256-item pass masks
	uint32_t mask_diff   = 0;
	uint64_t mask_items  = 0; // passing items summed over the compared tiles (how much was tested)
};

inline ListCompare CompareLists(const uint32_t* a, const uint32_t* b, uint32_t tiles) {
	ListCompare result;
	for (uint32_t tile = 0; tile < tiles; tile++) {
		const uint32_t* x = a + tile * kCheckEntryDwords;
		const uint32_t* y = b + tile * kCheckEntryDwords;
		if (x[0] == 0u || y[0] == 0u) {
			continue;
		}
		result.tiles++;
		bool mask_same = true;
		for (uint32_t i = 0; i < kCheckMaskDwords; i++) {
			mask_same = mask_same && x[kCheckWordMask + i] == y[kCheckWordMask + i];
			result.mask_items += static_cast<uint32_t>(__builtin_popcount(x[kCheckWordMask + i]));
		}
		(mask_same ? result.mask_equal : result.mask_diff)++;
		if (x[0] - 1u > kListEntries || y[0] - 1u > kListEntries) {
			result.overflowed++;
			continue;
		}
		if (x[0] != y[0]) {
			result.different++;
			if (result.first_diff < 0) result.first_diff = static_cast<int32_t>(tile);
			continue;
		}
		const uint32_t count = x[0] - 1u;
		bool           same  = true;
		for (uint32_t i = 0; i < count; i++) {
			same = same && x[1 + i] == y[1 + i];
		}
		if (same) {
			result.same_order++;
			continue;
		}
		// Same multiset in another order?
		bool used[kListEntries] = {};
		bool set_equal          = true;
		for (uint32_t i = 0; i < count && set_equal; i++) {
			bool found = false;
			for (uint32_t j = 0; j < count; j++) {
				if (!used[j] && x[1 + i] == y[1 + j]) {
					used[j] = true;
					found   = true;
					break;
				}
			}
			set_equal = found;
		}
		if (set_equal) {
			result.same_set++;
		} else {
			result.different++;
			if (result.first_diff < 0) result.first_diff = static_cast<int32_t>(tile);
		}
	}
	return result;
}

} // namespace Libs::Graphics::HalfRateTiles

#endif // GRAPHICS_SHADER_HALF_RATE_TILES_H
