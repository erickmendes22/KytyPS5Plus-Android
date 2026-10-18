#include "kernel/lazyReservation.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Kernel::LazyReservation::BackedBytes;
using Kernel::LazyReservation::IsOversized;
using Kernel::LazyReservation::Place;
using Kernel::LazyReservation::Window;

constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
constexpr uint64_t k2MiB = 2ull * 1024 * 1024;

// The ARM64 layout, spelled out so the test does not depend on the build host.
constexpr Window kArm64 {0x1000000000ull, 0x5f00000000ull, 0xfc00000000ull};

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LazyReservationTests: failed: %s\n", message);
		std::abort();
	}
}

struct Busy {
	uint64_t start;
	uint64_t end;
};

auto BusyEnd(const std::vector<Busy>& busy) {
	return [&busy](uint64_t start, uint64_t size) -> uint64_t {
		for (const auto& range: busy) {
			if (start < range.end && range.start < start + size) {
				return range.end;
			}
		}
		return 0;
	};
}

void TestUnrealReservationIsOversizedOnArm64() {
	// sceKernelReserveVirtualRange(0x1000000000, 512 GiB, flags 0, 2 MiB) from UE5 PS5 titles.
	Check(IsOversized(kArm64, 0x1000000000ull, 512 * kGiB), "512 GiB at 64 GiB is oversized");
	Check(!IsOversized(kArm64, 0x1000000000ull, 316 * kGiB), "the whole window is not oversized");
	Check(!IsOversized(kArm64, 0, 64 * kGiB), "a hint-less 64 GiB request is not oversized");
	Check(IsOversized(kArm64, 0x5e00000000ull, 8 * kGiB), "a request running past the window");
	Check(!IsOversized(kArm64, 0x5f00000000ull, 8 * kGiB),
	      "a hint above the window keeps the old behaviour");
}

void TestUnrealReservationPlacedAtHint() {
	const std::vector<Busy> busy {{0x200000000ull, 0x300000000ull}, {0x800000000ull, 0x920000000ull}};
	const auto start = Place(kArm64, 0x1000000000ull, 512 * kGiB, k2MiB, BusyEnd(busy));
	Check(start == 0x1000000000ull, "placed at the hint");
	Check(BackedBytes(kArm64, start, 512 * kGiB) == 0x4f00000000ull, "316 GiB of it is backed");
}

void TestPlacementSkipsBusyRanges() {
	const std::vector<Busy> busy {{0x1000000000ull, 0x1000204000ull}};
	const auto start = Place(kArm64, 0x1000000000ull, 512 * kGiB, k2MiB, BusyEnd(busy));
	Check(start == 0x1000400000ull, "moves past a busy range and re-aligns");
}

void TestPlacementRespectsCeiling() {
	Check(Place(kArm64, 0x1000000000ull, 0xf000000000ull, k2MiB, BusyEnd({})) == 0,
	      "a range ending above the guest ceiling is refused");
	Check(Place(kArm64, 0x1000000000ull, 0xec00000000ull, k2MiB, BusyEnd({})) == 0x1000000000ull,
	      "a range ending exactly at the guest ceiling fits");
	const std::vector<Busy> busy {{0x1000000000ull, 0x5f00000000ull}};
	Check(Place(kArm64, 0, 512 * kGiB, k2MiB, BusyEnd(busy)) == 0,
	      "no placement once the first page could not be backed");
}

void TestPlacementRejectsBadArguments() {
	Check(Place(kArm64, 0, 0, k2MiB, BusyEnd({})) == 0, "zero size");
	Check(Place(kArm64, 0, kGiB, 0, BusyEnd({})) == 0, "zero alignment");
	Check(Place(kArm64, 0, kGiB, 3 * k2MiB, BusyEnd({})) == 0, "non-power-of-two alignment");
	Check(Place(kArm64, UINT64_MAX - 10, kGiB, k2MiB, BusyEnd({})) == 0, "overflowing hint");
	const auto stuck = [](uint64_t start, uint64_t /*size*/) { return start; };
	Check(Place(kArm64, 0, 512 * kGiB, k2MiB, stuck) == 0, "a callback that does not advance");
}

void TestHintBelowFloorStartsAtFloor() {
	Check(Place(kArm64, 0x200000000ull, 512 * kGiB, k2MiB, BusyEnd({})) == 0x1000000000ull,
	      "a low hint starts at the user window");
}

void TestBackedBytes() {
	Check(BackedBytes(kArm64, 0x5f00000000ull, kGiB) == 0, "nothing backed above the window");
	Check(BackedBytes(kArm64, 0x5e00000000ull, kGiB) == kGiB, "fully backed");
	Check(BackedBytes(kArm64, 0x5e00000000ull, 8 * kGiB) == 4 * kGiB, "partly backed");
}

void TestHostWindowMatchesLayout() {
	constexpr auto window = Kernel::LazyReservation::HostWindow();
	static_assert(window.floor == Common::AddressLayout::kHostUserMin);
	static_assert(window.start_limit == Common::AddressLayout::kHostUserEnd);
	static_assert(window.ceiling == Common::AddressLayout::kGuestUserEnd);
#if KYTY_HOST_ARM64
	static_assert(window.start_limit == kArm64.start_limit && window.floor == kArm64.floor);
	Check(IsOversized(window, 0x1000000000ull, 512 * kGiB), "UE5 request is oversized on ARM64");
#else
	Check(!IsOversized(window, 0x1000000000ull, 512 * kGiB),
	      "UE5 request fits the x86 host window");
#endif
}

} // namespace

int main() {
	TestUnrealReservationIsOversizedOnArm64();
	TestUnrealReservationPlacedAtHint();
	TestPlacementSkipsBusyRanges();
	TestPlacementRespectsCeiling();
	TestPlacementRejectsBadArguments();
	TestHintBelowFloorStartsAtFloor();
	TestBackedBytes();
	TestHostWindowMatchesLayout();
	std::printf("LazyReservationTests: all passed\n");
	return 0;
}
