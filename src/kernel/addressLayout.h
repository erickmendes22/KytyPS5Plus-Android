#ifndef EMULATOR_SRC_KERNEL_ADDRESSLAYOUT_H_
#define EMULATOR_SRC_KERNEL_ADDRESSLAYOUT_H_

#include "common/hostArch.h"

#include <cstdint>

// Single source of truth for the guest/host virtual address layout.
//
// x86-64 hosts keep the values the emulator has always used; every x86 constant below must stay
// exactly as it is.
//
// ARM64 hosts (Android kernels) give an app a 39-bit address space: nothing can be mapped at or
// above 0x8000000000, and the top of it is taken by the process stack and libraries, so the usable
// top is 0x7F00000000. The ARM64 layout keeps every guest-mandated address (default map base
// 0x200000000, modules at 0x800000000/0x900000000, DriverDmem 0x0FE0000000, stacks below 32 GiB)
// and packs everything else below 0x7F00000000:
//
//   ARM64 range                         start             end               size
//   host system (reserved)              0x0000040000      0x1000000000      ~64 GiB
//   host user window                    0x1000000000      0x5F00000000      380 GiB - 64 GiB
//   low arena (sub-range of user)       0x3000000000      0x5F00000000      188 GiB
//   PRT aperture (sub-range of user)    0x0F00000000      0x5F00000000
//   unused gap                          0x5F00000000      0x6000000000      4 GiB
//   guest extended memory               0x6000000000      0x7000000000      64 GiB (x86: 512 GiB)
//   ASAN shadow for [0, 0x7000000000)   0x7000000000      0x7E00000000      56 GiB (debug only)
//   GPU page tracker window             0x0000000000      0x8000000000      512 GiB
//
// The extended-memory size shrinks from 512 GiB (x86) to 64 GiB (ARM64) so the region fits below
// the usable top; it is reported to guests through AmmGetVirtualAddressRanges.

namespace Common::AddressLayout {

#if KYTY_HOST_ARM64
inline constexpr uint64_t kUsableTop = 0x7F00000000ull;
#endif

// ---- Host user window (guest-visible reservations) ----------------------------------------------

#if KYTY_HOST_ARM64
inline constexpr uint64_t kHostSystemManagedMin  = 0x0000040000ull;
inline constexpr uint64_t kHostSystemManagedMax  = 0x07ffffbfffull;
inline constexpr uint64_t kHostSystemReservedMax = 0x0fffffffffull;
inline constexpr uint64_t kHostUserMin           = 0x1000000000ull;
inline constexpr uint64_t kHostUserEnd           = 0x5f00000000ull;
#elif defined(__APPLE__)
inline constexpr uint64_t kHostSystemManagedMin  = 0x0000040000ull;
inline constexpr uint64_t kHostSystemManagedMax  = 0x07ffffbfffull;
inline constexpr uint64_t kHostSystemReservedMax = 0x0fbfffffffull;
inline constexpr uint64_t kHostUserMin           = 0x7000000000ull;
inline constexpr uint64_t kHostUserEnd           = 0xfc00000000ull;
#else
inline constexpr uint64_t kHostSystemManagedMin  = 0x0000040000ull;
inline constexpr uint64_t kHostSystemManagedMax  = 0x07ffffbfffull;
inline constexpr uint64_t kHostSystemReservedMax = 0x0fffffffffull;
inline constexpr uint64_t kHostUserMin           = 0x1000000000ull;
inline constexpr uint64_t kHostUserEnd           = 0xfc00000000ull;
#endif
inline constexpr uint64_t kHostUserMax = kHostUserEnd - 1;

// ---- Guest extended memory (AMM) ----------------------------------------------------------------

#if KYTY_HOST_ARM64
inline constexpr uint64_t kExtendedMemoryBase = 0x6000000000ull;
inline constexpr uint64_t kExtendedMemorySize = 64ull * 1024 * 1024 * 1024;
#else
inline constexpr uint64_t kExtendedMemoryBase = 0x080000000000ull;
inline constexpr uint64_t kExtendedMemorySize = 512ull * 1024 * 1024 * 1024;
#endif
inline constexpr uint64_t kExtendedMemoryEnd = kExtendedMemoryBase + kExtendedMemorySize;

struct VirtualRange {
	uint64_t start;
	uint64_t end;
};

// Range reported to guests by AmmGetVirtualAddressRanges.
[[nodiscard]] constexpr VirtualRange ExtendedVirtualRange() noexcept {
	return {kExtendedMemoryBase, kExtendedMemoryEnd};
}

// ---- GPU page tracker / buffer cache address windows ---------------------------------------------

// Bits of guest address space covered by the GPU page tracker and buffer-cache page table.
#if KYTY_HOST_ARM64
inline constexpr uint32_t kTrackerAddressBits = 39;
#else
inline constexpr uint32_t kTrackerAddressBits = 44;
#endif
inline constexpr uint64_t kTrackerAddressSize = uint64_t {1} << kTrackerAddressBits;

// Addresses below this value form the "lower" GPU window; addresses in
// [kLowerAddressSize, kExtendedMemoryBase) are not guest-valid. On ARM64 the lower window runs up
// to the extended base because every non-extended guest address (up to kHostUserEnd) must stay
// valid, so the extended window directly follows it and the GPU fold offset is zero.
#if KYTY_HOST_ARM64
inline constexpr uint64_t kLowerAddressSize = kExtendedMemoryBase;
#else
inline constexpr uint64_t kLowerAddressSize = uint64_t {1} << 40u;
#endif

// Offset subtracted from extended addresses to pack them directly after the lower window.
inline constexpr uint64_t kExtendedFoldOffset = kExtendedMemoryBase - kLowerAddressSize;

// ---- Host-side automatic mapping arena ----------------------------------------------------------

inline constexpr uint64_t kLowArenaLimit = kHostUserEnd;
#if KYTY_HOST_ARM64
inline constexpr uint64_t kLowArenaFloor = 0x3000000000ull;
#else
inline constexpr uint64_t kLowArenaFloor = 0xa000000000ull;
#endif

// ---- GPU PRT aperture ---------------------------------------------------------------------------

inline constexpr uint64_t kPrtApertureStart = 0x0f00000000ull;
inline constexpr uint64_t kPrtApertureEnd   = kHostUserEnd;

// ---- Guest application address limit (libAmpr buffer validation) --------------------------------

#if KYTY_HOST_ARM64
inline constexpr uint64_t kMaxAppAddress = kExtendedMemoryEnd;
#else
inline constexpr uint64_t kMaxAppAddress = 0x0000f00000000000ull;
#endif

// ---- ASAN debug shadow --------------------------------------------------------------------------

// Shadow of address A lives at (A >> 3) + kAsanShadowOffset.
#if KYTY_HOST_ARM64
inline constexpr uint64_t kAsanShadowOffset = kExtendedMemoryEnd;
#else
inline constexpr uint64_t kAsanShadowOffset = 0x10000000000ull;
#endif

// ---- Compile-time invariants --------------------------------------------------------------------

static_assert(kHostSystemManagedMin < kHostSystemManagedMax);
static_assert(kHostSystemReservedMax < kHostUserMin);
static_assert(kHostUserMin < kHostUserEnd);
static_assert(kHostUserEnd <= kExtendedMemoryBase, "user window must not overlap extended memory");
static_assert(kExtendedMemoryEnd <= kTrackerAddressSize,
              "extended memory must lie inside the GPU page tracker window");
static_assert(kLowerAddressSize >= kHostUserEnd, "every user address must be in the lower window");
static_assert(kLowerAddressSize <= kExtendedMemoryBase);
static_assert(kLowArenaFloor < kLowArenaLimit && kLowArenaLimit <= kLowerAddressSize,
              "arena must sit inside the user window and the lower GPU window");
static_assert(kPrtApertureStart < kPrtApertureEnd && kPrtApertureEnd <= kHostUserEnd);
static_assert(kExtendedFoldOffset <= kExtendedMemoryBase);
#if KYTY_HOST_ARM64
static_assert(kExtendedMemoryEnd <= kUsableTop, "extended memory must end below the usable top");
static_assert((kExtendedMemoryEnd >> 3) + kAsanShadowOffset <= kUsableTop,
              "ASAN shadow for the guest address space must fit below the usable top");
static_assert(kHostUserEnd <= 0x7F00000000ull, "user window must end below the usable top");
#endif

} // namespace Common::AddressLayout

#endif // EMULATOR_SRC_KERNEL_ADDRESSLAYOUT_H_
