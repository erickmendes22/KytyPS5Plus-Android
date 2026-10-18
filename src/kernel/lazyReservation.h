#ifndef EMULATOR_SRC_KERNEL_LAZYRESERVATION_H_
#define EMULATOR_SRC_KERNEL_LAZYRESERVATION_H_

#include "kernel/addressLayout.h"

#include <algorithm>
#include <cstdint>

// Placement of guest virtual-address reservations that the host cannot back in full.
//
// Guest addresses are host addresses (FEXCore runs guest code with identity-mapped memory), so a
// guest page can only be committed where the host owns the same address. A reservation itself is
// bookkeeping (the host window is PROT_NONE-reserved once at startup), so a reservation larger
// than the host window can still be granted: the part inside the host user window is backed as
// usual, and the part above it is recorded but commits there fail cleanly (except inside the
// extended/AMM window, which the host also owns).
//
// Unreal Engine 5 reserves 512 GiB at 0x1000000000 for its allocator on PS5. An ARM64 host with
// a 39-bit address space has a 316 GiB user window (0x1000000000..0x5F00000000), so the top of
// that reservation can never be committed. That works as long as the game commits only the part
// the host can back.
//
// Only requests that cannot fit in the backed window at all take this path, so every reservation
// that worked before keeps exactly its old placement.

namespace Kernel::LazyReservation {

struct Window {
	uint64_t floor;       // lowest start address for a lazy reservation
	uint64_t start_limit; // a lazy reservation must start below this (its first page is backed)
	uint64_t ceiling;     // a lazy reservation must end at or below this
};

// The host-backed user window, with the guest's user ceiling above it.
[[nodiscard]] constexpr Window HostWindow() noexcept {
	return {Common::AddressLayout::kHostUserMin, Common::AddressLayout::kHostUserEnd,
	        Common::AddressLayout::kGuestUserEnd};
}

[[nodiscard]] constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
	if (alignment == 0 || value > UINT64_MAX - (alignment - 1)) {
		return 0;
	}
	return (value + alignment - 1) / alignment * alignment;
}

// True when no placement at or above the hint could fit inside [floor, start_limit), so the request
// can only be granted with an unbacked tail.
[[nodiscard]] constexpr bool IsOversized(const Window& window, uint64_t hint,
                                         uint64_t size) noexcept {
	const uint64_t start = std::max(hint, window.floor);
	if (start >= window.start_limit) {
		return false;
	}
	return size > window.start_limit - start;
}

// Number of bytes of [start, start + size) that lie below the backed limit.
[[nodiscard]] constexpr uint64_t BackedBytes(const Window& window, uint64_t start,
                                             uint64_t size) noexcept {
	if (start >= window.start_limit) {
		return 0;
	}
	return std::min(size, window.start_limit - start);
}

// Lowest aligned start >= max(hint, floor) such that [start, start + size) ends at or below the
// ceiling and overlaps no existing guest range. busy_end(start, size) returns 0 when the span is
// free, otherwise the end of a guest range that overlaps it. Returns 0 when there is no placement.
template <typename BusyEnd>
[[nodiscard]] uint64_t Place(const Window& window, uint64_t hint, uint64_t size,
                             uint64_t alignment, BusyEnd&& busy_end) {
	if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
		return 0;
	}
	uint64_t candidate = std::max(hint, window.floor);
	// Each step moves past one overlapping guest range; the bound only guards a broken callback.
	for (int step = 0; step < 1 << 16; step++) {
		candidate = AlignUp(candidate, alignment);
		if (candidate == 0 || candidate >= window.start_limit || candidate > window.ceiling ||
		    size > window.ceiling - candidate) {
			return 0;
		}
		const uint64_t end = busy_end(candidate, size);
		if (end == 0) {
			return candidate;
		}
		if (end <= candidate) {
			return 0;
		}
		candidate = end;
	}
	return 0;
}

} // namespace Kernel::LazyReservation

#endif // EMULATOR_SRC_KERNEL_LAZYRESERVATION_H_
