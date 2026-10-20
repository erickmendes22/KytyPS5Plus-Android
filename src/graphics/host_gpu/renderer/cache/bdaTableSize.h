#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDATABLESIZE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDATABLESIZE_H_

#include <algorithm>
#include <cstdint>

// KYTY_BDA_TABLE (BufferCache): the BDA page table holds one 8-byte entry per 16 KiB page. Dense,
// it covers the whole GPU-addressable range (224 MiB on ARM64 for 448 GiB); sized, it covers only
// up to the highest page a guest mapping or a registered buffer reaches, rounded up to a granule,
// and grows when that moves. Shaders index it with robustBufferAccess2, so a page past the end
// reads as zero (missing) and takes the BDA fault path, which registers the buffer and grows the
// table.
namespace Libs::Graphics::BdaTableSize {

// 8 GiB of guest address per granule = 4 MiB of table.
inline constexpr uint64_t kGranulePages = uint64_t {1} << 19u;

// Table pages to allocate so that `needed_pages` fit: rounded up to a granule, at least one
// granule, at most `max_pages`.
[[nodiscard]] constexpr uint64_t PagesFor(uint64_t needed_pages, uint64_t max_pages) {
	const auto granules = (std::max<uint64_t>(needed_pages, 1) + kGranulePages - 1) / kGranulePages;
	return std::min(granules * kGranulePages, max_pages);
}

// The table size after a request for `needed_pages` with `current_pages` allocated: unchanged when
// they fit, else PagesFor (never shrinks).
[[nodiscard]] constexpr uint64_t GrowTo(uint64_t current_pages, uint64_t needed_pages, uint64_t max_pages) {
	return needed_pages <= current_pages ? current_pages
	                                     : std::max(current_pages, PagesFor(needed_pages, max_pages));
}

// KYTY_BDA_TABLE: "dense" keeps the full-size table; anything else (default) sizes it when the
// device zeroes out-of-range storage reads (robustBufferAccess2 with 4-byte alignment).
[[nodiscard]] constexpr bool Sized(const char* env, bool robust_storage_reads) {
	const bool dense = env != nullptr && env[0] == 'd' && env[1] == 'e' && env[2] == 'n' && env[3] == 's' &&
	                   env[4] == 'e' && env[5] == '\0';
	return robust_storage_reads && !dense;
}

} // namespace Libs::Graphics::BdaTableSize

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDATABLESIZE_H_
