#ifndef EMULATOR_SRC_KERNEL_ADDRESSFOLD_H_
#define EMULATOR_SRC_KERNEL_ADDRESSFOLD_H_

#include "kernel/addressLayout.h"

#include <algorithm>
#include <cstdint>

// Guest-to-host address folding (ARM64 hosts with a 39-bit user address space).
//
// Guest addresses are host addresses, so a guest range can only be committed where the host can
// map the same address. Unreal Engine 5 reserves 512 GiB on the PS5 and allocates from both ends
// of it; on a 39-bit host no placement of such a range fits, and its top end lies above 2^39.
//
// Folding maps guest address A to host address A & (2^39 - 1) for guest addresses in
// [2^39, 2^40) ("the fold domain") and leaves every other address unchanged. It is enabled only
// once the kernel grants a reservation that reaches into the fold domain (FoldPlan below); from
// then on:
//   - FEXCore folds the address of every guest memory access (FEX patch fex-addressfold.patch),
//   - the kernel memory calls map, protect and release the folded host address while the guest
//     keeps the unfolded one (g_virtual_ranges holds guest addresses),
//   - host code that dereferences a guest pointer from the fold domain faults and has the
//     pointer register folded by the fault handler, and system calls that take guest buffers
//     (file I/O, futexes) fold the pointer first.
// Below 2^39 folding is the identity, so nothing changes for games that never reach the domain.

namespace Kernel::AddressFold {

inline constexpr uint32_t kFoldBits   = 39;
inline constexpr uint64_t kFoldSize   = uint64_t {1} << kFoldBits;
inline constexpr uint64_t kFoldMask   = kFoldSize - 1;
inline constexpr uint64_t kDomainEnd  = kFoldSize * 2;

// True for a guest address that folding moves.
[[nodiscard]] constexpr bool InDomain(uint64_t guest) noexcept {
	return guest >= kFoldSize && guest < kDomainEnd;
}

// Host address backing guest address `guest`.
[[nodiscard]] constexpr uint64_t Fold(uint64_t guest) noexcept {
	return InDomain(guest) ? guest & kFoldMask : guest;
}

// Folds [guest, guest + size) into one contiguous host span. Fails for an empty span, one that
// wraps, or one that straddles 2^39 (its two halves would land at opposite ends of host VA).
[[nodiscard]] constexpr bool FoldSpan(uint64_t guest, uint64_t size, uint64_t* host) noexcept {
	if (size == 0 || guest > UINT64_MAX - size) {
		return false;
	}
	const uint64_t last = guest + size - 1;
	if (InDomain(guest) != InDomain(last) || (guest < kFoldSize) != (last < kFoldSize)) {
		return false;
	}
	*host = Fold(guest);
	return true;
}

template <typename T>
[[nodiscard]] T* FoldPointer(T* guest) noexcept {
	return reinterpret_cast<T*>(Fold(reinterpret_cast<uint64_t>(guest)));
}

// Placement of a reservation that reaches into the fold domain.
//
// The part of [start, start + size) at or above 2^39 folds to [0, start + size - 2^39). The plan
// puts that folded image's top `alias_size` bytes at the bottom of the host user window, below the
// reservation itself:
//
//   guest:  [alias_start, alias_end)  never handed out to the guest (the folded top lives there)
//           [start, start + size)     the reservation; [2^39, start + size) folds onto the alias
//
// so the top of the reservation (where UE5 allocates top-down) is backed by free host VA, and its
// bottom by the rest of the user window. The middle of the reservation (above the host user
// window and below 2^39, plus everything that folds below the alias) stays unbacked: commits there
// fail as with lazy reservations.
struct FoldPlan {
	uint64_t start       = 0; // reservation start (guest address)
	uint64_t alias_start = 0; // host = guest range reserved for the folded top
	uint64_t alias_end   = 0;
};

// window_start/window_end: host user window. alias_size: wanted backing for the folded top.
// alignment: power of two. Returns false when the request does not reach into the fold domain
// from the window, or does not fit.
[[nodiscard]] constexpr bool PlanFold(uint64_t size, uint64_t alignment, uint64_t window_start,
                                      uint64_t window_end, uint64_t alias_size,
                                      FoldPlan* plan) noexcept {
	if (size == 0 || size > kFoldSize || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    alias_size == 0 || window_start >= window_end) {
		return false;
	}
	// The folded image of the top is [0, start + size - 2^39); its last alias_size bytes must
	// start at window_start: start + size - 2^39 = window_start + alias_size.
	const uint64_t raw_start = window_start + alias_size + (kFoldSize - size);
	const uint64_t start     = (raw_start + alignment - 1) & ~(alignment - 1);
	if (start >= window_end || start + size <= kFoldSize || start + size > kDomainEnd) {
		return false;
	}
	plan->start       = start;
	plan->alias_start = window_start;
	plan->alias_end   = start + size - kFoldSize;
	return plan->alias_end > plan->alias_start && plan->alias_end <= start;
}

// The plan for this host, with a 32 GiB folded top (UE5's top-down allocations; the PS5 has 16 GiB
// of memory in total).
inline constexpr uint64_t kAliasSize = 32ull << 30u;

[[nodiscard]] constexpr bool PlanHostFold(uint64_t size, uint64_t alignment, FoldPlan* plan) noexcept {
	return PlanFold(size, alignment, Common::AddressLayout::kHostUserMin,
	                Common::AddressLayout::kHostUserEnd, kAliasSize, plan);
}

// Runtime switch, off until the first folded reservation (see memory.cpp). IsEnabled is cheap
// enough for hot paths; Fold itself does not depend on it, since it is the identity for every
// address the guest can use while folding is off.
bool IsEnabled() noexcept;
void SetEnabled() noexcept;

} // namespace Kernel::AddressFold

#endif // EMULATOR_SRC_KERNEL_ADDRESSFOLD_H_
