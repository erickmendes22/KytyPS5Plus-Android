#ifndef GRAPHICS_SHADER_HALF_RATE_TILES_H
#define GRAPHICS_SHADER_HALF_RATE_TILES_H

// KYTY_RT_HALF=1 / KYTY_LIVE_KNOBS rt_half=0|1 (experiment, off by default): Astro Bot's tiled
// deferred-lighting kernel CS 6856320a (research/NEXT.md "c17_rtC") shades one pixel in four.
// The game's resolution and every other pass are unchanged.
//
// The kernel runs one 16x16 workgroup per screen tile from a GPU-built tile list. Its prologue
// culls lights cooperatively over all 256 threads (LDS append, two barriers), so the group size
// stays as it is. After the prologue (guest pc 0x4dc, where the pixel coordinates are formed),
// wave 0 (threads 0..63) takes the even/even pixels of the tile, and waves 1-3 get coordinates
// that fail the kernel's own bounds check, so they skip the shading. Wave 0 also saves its
// samples (depth, G-buffer value, lit color, luminance) to LDS added after the guest's. Before
// s_endpgm, all 256 threads pass a workgroup barrier and fill the 3 other pixels of each 2x2
// from the up-to-4 nearest samples in the same tile. The weights are bilinear, times depth and
// G-buffer similarity (a joint bilateral upsample). Then they store through the guest's own
// output descriptors.
//
// Only the listed hash with the expected instructions at the patch points is changed. Other
// shaders, or a mismatch, compile as before (the recompiler logs the reason).

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::HalfRateTiles {

inline constexpr uint64_t kShaderHash = 0x6856320a9c6f89fdull;

// LDS words appended after the guest's: 11 arrays of 64 words (valid flag, depth, 4 G-buffer
// words, 4 color words, luminance).
inline constexpr uint32_t kSamples    = 64;
inline constexpr uint32_t kArrays     = 11;
inline constexpr uint32_t kLdsDwords  = kSamples * kArrays;

// Guest pcs of 6856320a used by the patch.
inline constexpr uint32_t kPcCoordX       = 0x4d8; // V_ADD_NC_U32 v24, s12, v0
inline constexpr uint32_t kPcCoordY       = 0x4dc; // V_ADD_NC_U32 v25, s13, v1
inline constexpr uint32_t kPcBoundsCmp    = 0x500; // V_CMP_LE_U32 s10, s13, v25 (s12/s13 = extent)
inline constexpr uint32_t kPcDepthDesc    = 0x51c; // S_LOAD_DWORDX8 s16, s14 + 128
inline constexpr uint32_t kPcGbufferDesc  = 0x524; // S_LOAD_DWORDX8 s24, s14 + 0
inline constexpr uint32_t kPcDepthLoad    = 0x534; // IMAGE_LOAD v26, v24, s16 (R32 depth)
inline constexpr uint32_t kPcGbufferLoad  = 0x53c; // IMAGE_LOAD v3..v6, v24, s24
inline constexpr uint32_t kPcColorDesc    = 0x41bc; // S_LOAD_DWORDX8 s0, s14 + 448
inline constexpr uint32_t kPcColorStore   = 0x4228; // IMAGE_STORE v3..v6, v24, s0 (RGBA16F)
inline constexpr uint32_t kPcLumaDesc     = 0x425c; // S_LOAD_DWORDX8 s0, s14 + 480
inline constexpr uint32_t kPcLumaStore    = 0x4268; // IMAGE_STORE v8, v24, s0 (R16F)
inline constexpr uint32_t kPcEnd          = 0x4270; // s_endpgm

// Spare VGPRs (the kernel uses v0..v71) carrying the tile origin and the screen extent from the
// prologue to the end.
inline constexpr uint32_t kRegOriginX = 240;
inline constexpr uint32_t kRegOriginY = 241;
inline constexpr uint32_t kRegExtentX = 242;
inline constexpr uint32_t kRegExtentY = 243;

// live: 0 = KYTY_RT_HALF decides, 1 = off, 2 = on.
inline bool EnvEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_RT_HALF");
		return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
	}();
	return enabled;
}

inline bool Applies(uint64_t hash, uint8_t live) {
	if (hash != kShaderHash) {
		return false;
	}
	return live == 0 ? EnvEnabled() : live == 2;
}

} // namespace Libs::Graphics::HalfRateTiles

#endif // GRAPHICS_SHADER_HALF_RATE_TILES_H
