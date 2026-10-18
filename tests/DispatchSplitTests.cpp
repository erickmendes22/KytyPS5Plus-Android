#include "graphics/host_gpu/renderer/dispatchSplit.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Libs::Graphics::DispatchFits;
using Libs::Graphics::DispatchGroups;
using Libs::Graphics::DispatchPiece;
using Libs::Graphics::ForEachDispatchPiece;

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", what);
		g_failures++;
	}
}

std::vector<DispatchPiece> Pieces(const DispatchGroups& groups, const DispatchGroups& limits) {
	std::vector<DispatchPiece> pieces;
	const auto count =
	    ForEachDispatchPiece(groups, limits, [&](const DispatchPiece& piece) { pieces.push_back(piece); });
	Check(count == pieces.size(), "returned count matches the emitted pieces");
	return pieces;
}

constexpr DispatchGroups kAdreno {65535, 65535, 65535};

void TestFittingDispatchIsOnePiece() {
	Check(DispatchFits({65535, 1, 1}, kAdreno), "65535 fits");
	const auto pieces = Pieces({640, 3, 2}, kAdreno);
	Check(pieces.size() == 1, "fitting dispatch is one piece");
	Check(pieces[0].base == DispatchGroups {0, 0, 0}, "fitting piece has no base");
	Check(pieces[0].count == DispatchGroups {640, 3, 2}, "fitting piece keeps the counts");
}

void TestAstroGuestDispatch() {
	// cs 338b4505 / 3a8772db / 9e862738: 131072 x 1 x 1.
	Check(!DispatchFits({131072, 1, 1}, kAdreno), "131072 does not fit");
	const auto pieces = Pieces({131072, 1, 1}, kAdreno);
	Check(pieces.size() == 3, "131072 groups take three pieces");
	Check(pieces[0].base[0] == 0 && pieces[0].count[0] == 65535, "first piece");
	Check(pieces[1].base[0] == 65535 && pieces[1].count[0] == 65535, "second piece");
	Check(pieces[2].base[0] == 131070 && pieces[2].count[0] == 2, "last piece is the remainder");
}

void TestEveryGroupExactlyOnce() {
	const DispatchGroups groups {10, 7, 5};
	const DispatchGroups limits {4, 3, 2};
	std::vector<int> seen(10 * 7 * 5, 0);
	for (const auto& piece: Pieces(groups, limits)) {
		for (uint32_t axis = 0; axis < 3; axis++) {
			Check(piece.count[axis] >= 1 && piece.count[axis] <= limits[axis], "piece within limits");
		}
		for (uint32_t z = 0; z < piece.count[2]; z++) {
			for (uint32_t y = 0; y < piece.count[1]; y++) {
				for (uint32_t x = 0; x < piece.count[0]; x++) {
					const auto gx = piece.base[0] + x;
					const auto gy = piece.base[1] + y;
					const auto gz = piece.base[2] + z;
					seen[(gz * 7 + gy) * 10 + gx]++;
				}
			}
		}
	}
	bool once = true;
	for (const auto value: seen) {
		once = once && value == 1;
	}
	Check(once, "every group is dispatched exactly once");
}

// The LDS-in-storage fallback indexes its slot with the piece's gl_NumWorkGroups; within a piece
// the index must stay unique and inside the buffer sized for the whole dispatch.
void TestLdsSlotIndexWithinPiece() {
	const DispatchGroups groups {9, 4, 3};
	const DispatchGroups limits {4, 3, 2};
	const uint32_t       total = 9 * 4 * 3;
	bool ok = true;
	for (const auto& piece: Pieces(groups, limits)) {
		std::vector<int> used(total, 0);
		for (uint32_t z = 0; z < piece.count[2]; z++) {
			for (uint32_t y = 0; y < piece.count[1]; y++) {
				for (uint32_t x = 0; x < piece.count[0]; x++) {
					const auto gx  = piece.base[0] + x;
					const auto gy  = piece.base[1] + y;
					const auto gz  = piece.base[2] + z;
					const auto row = gy + gz * piece.count[1];
					const auto index = gx + row * piece.count[0];
					ok = ok && index < total && used[index]++ == 0;
				}
			}
		}
	}
	Check(ok, "LDS slot index unique and in range within each piece");
}

void TestEmptyAndZeroLimit() {
	Check(Pieces({0, 5, 1}, kAdreno).empty(), "empty dispatch has no pieces");
	Check(Pieces({3, 1, 1}, {0, 1, 1}).size() == 3, "a zero limit counts as one group per piece");
}

} // namespace

int main() {
	TestFittingDispatchIsOnePiece();
	TestAstroGuestDispatch();
	TestEveryGroupExactlyOnce();
	TestLdsSlotIndexWithinPiece();
	TestEmptyAndZeroLimit();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("dispatch_split_tests: all passed\n");
	return EXIT_SUCCESS;
}
