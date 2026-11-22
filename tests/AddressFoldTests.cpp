#include "kernel/addressFold.h"
#include "libs/aprReadRange.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Kernel::AddressFold;

constexpr uint64_t kGiB  = 1024ull * 1024 * 1024;
constexpr uint64_t k2MiB = 2ull * 1024 * 1024;

// ARM64 host user window, spelled out so the test does not depend on the build host.
constexpr uint64_t kWindowStart = 0x1000000000ull;
constexpr uint64_t kWindowEnd   = 0x5f00000000ull;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AddressFoldTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFoldIsIdentityBelowTheBoundary() {
	static_assert(kFoldSize == 0x8000000000ull);
	static_assert(Fold(0) == 0);
	static_assert(Fold(0x200000000ull) == 0x200000000ull);
	static_assert(Fold(kFoldSize - 1) == kFoldSize - 1);
	// Tagged host heap pointers (Android TBI tag in the top byte) are not in the domain.
	static_assert(Fold(0xb400007fffff0000ull) == 0xb400007fffff0000ull);
	static_assert(Fold(kDomainEnd) == kDomainEnd);
	Check(!InDomain(kFoldSize - 1) && InDomain(kFoldSize) && InDomain(kDomainEnd - 1) &&
	          !InDomain(kDomainEnd),
	      "domain bounds");
}

void TestFoldMovesTheDomain() {
	static_assert(Fold(kFoldSize) == 0);
	static_assert(Fold(0x8fc0000000ull) == 0x0fc0000000ull);
	static_assert(Fold(0x97ffffc000ull) == 0x17ffffc000ull);
        // My First Gran Turismo's fixed 16 KiB counter page.
        static_assert(Fold(0xe3ffffc000ull) == 0x63ffffc000ull);
        int  local = 0;
	auto host  = reinterpret_cast<uint64_t>(&local);
	Check(FoldPointer(&local) == &local, "host pointers are unchanged");
	Check(Fold(host) == host, "host addresses are unchanged");
	// MFGT's scheduler compares FiberGetSelf with the ORIGINAL guest object array.
	// Returning the folded backing pointer underflows this exact observed range.
	constexpr uint64_t fiber_base = 0xcc00284000ull;
	constexpr uint64_t fiber_self = 0xcc00284300ull;
	Check((fiber_self - fiber_base) / 256 == 3, "guest fiber identity selects scheduler slot 3");
	Check((Fold(fiber_self) - fiber_base) / 256 > 0x3c000,
	      "folded fiber identity fails the game's unsigned scheduler bounds check");
}

void TestFoldSpan() {
	uint64_t host = 0;
	Check(FoldSpan(0x8fc0000000ull, 32ull << 20u, &host) && host == 0x0fc0000000ull, "span in domain");
	Check(FoldSpan(0x200000000ull, kGiB, &host) && host == 0x200000000ull, "span below");
        Check(FoldSpan(0xe3ffffc000ull, 0x4000, &host) &&
                  host == 0x63ffffc000ull,
              "My First Gran Turismo fixed mapping fits the host AMM window");
        Check(!FoldSpan(kFoldSize - 0x4000, 0x8000, &host), "span straddling 2^39");
	Check(!FoldSpan(kDomainEnd - 0x4000, 0x8000, &host), "span straddling 2^40");
	// PPSA24156: AvPlayer allocates its NV12 output at a guest VA above 2^39.
	// Native FFmpeg writes must use the folded backing, but callback/frame pointers
	// must keep the original address. Cover the whole buffer rather than only byte 0.
	constexpr uint64_t video_guest = 0xd41b2fc000ull;
	constexpr uint64_t video_bytes = 8ull * 1024 * 1024;
	Check(FoldSpan(video_guest, video_bytes, &host) && host == 0x541b2fc000ull,
	      "MFGT AvPlayer 8 MiB native output is within one folded host region");
	Check(reinterpret_cast<uint64_t>(FoldPointer(reinterpret_cast<uint8_t*>(video_guest))) == host &&
	          video_guest != host,
	      "native video write uses folded backing and guest identity remains distinct");
	Check(!FoldSpan(kDomainEnd - 4096, video_bytes, &host),
	      "native video output must not cross the 2^40 fold boundary");
	Check(!FoldSpan(0x1000, 0, &host), "empty span");
	Check(!FoldSpan(UINT64_MAX - 10, 0x100, &host), "wrapping span");
}

void TestUnrealPlan() {
	// sceKernelReserveVirtualRange(0x1000000000, 512 GiB, flags 0, 2 MiB) with a 32 GiB alias.
	FoldPlan plan {};
	Check(PlanFold(512 * kGiB, k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &plan), "UE5 plan");
	Check(plan.start == 0x1800000000ull, "reservation starts above the alias");
	Check(plan.alias_start == kWindowStart && plan.alias_end == plan.start, "alias below the range");
	// The top 32 GiB of the range folds exactly onto the alias.
	const uint64_t top = plan.start + 512 * kGiB - 32 * kGiB;
	Check(Fold(top) == plan.alias_start && Fold(plan.start + 512 * kGiB - 1) == plan.alias_end - 1,
	      "top folds onto the alias");
	// The bottom of the range is backed by the host window as is.
	Check(Fold(plan.start) == plan.start && plan.start < kWindowEnd, "bottom is host-backed");
	// UE5's measured top allocation (32 MiB at offset 511 GiB from its base).
	Check(Fold(plan.start + 511 * kGiB) >= plan.alias_start &&
	          Fold(plan.start + 511 * kGiB + (32ull << 20u)) <= plan.alias_end,
	      "UE5 top allocation lands in the alias");
}

void TestSarosObservedFoldedDirectMapping() {
	// Captured PPSA07631 #644: initial protection covered 32 GiB, yet
	// sceKernelBatchMap mapped guest 0x98c0000000 (+192 MiB) to host
	// 0x18c0000000 and aborted with address-range-unavailable.
	// The guest mapping must lie inside *both* the 512 GiB reservation and the
	// real host alias. A larger reserved alias is not an arbitrary host-memory
	// allocation: the host only reserves VA with PROT_NONE / MAP_NORESERVE.
	constexpr uint64_t guest = 0x98c0000000ull;
	constexpr uint64_t bytes = 192ull << 20u;
	constexpr uint64_t host = 0x18c0000000ull;
	constexpr uint64_t saros_alias = 48 * kGiB;
	FoldPlan plan {};
	Check(PlanFold(512 * kGiB, k2MiB, kWindowStart, kWindowEnd, saros_alias, &plan),
	      "Saros 48 GiB fold placement exists");
	Check(plan.start == 0x1c00000000ull &&
	          plan.start + 512 * kGiB == 0x9c00000000ull,
	      "Saros planned guest arena includes captured fixed direct mapping");
	uint64_t mapped = 0;
	Check(FoldSpan(guest, bytes, &mapped) && mapped == host,
	      "Saros captured 192 MiB guest mapping folds to the host");
	Check(guest >= plan.start && guest + bytes <= plan.start + 512 * kGiB,
	      "Saros captured direct mapping is inside its guest reservation");
	Check(mapped >= plan.alias_start && mapped + bytes <= plan.alias_end,
	      "Saros captured direct mapping has protected host alias");
	FoldPlan old_plan {};
	Check(PlanFold(512 * kGiB, k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &old_plan),
	      "original 32 GiB fold exists");
	Check(guest >= old_plan.start + 512 * kGiB &&
	          mapped >= old_plan.alias_end,
	      "original 32 GiB plan excluded the observed crash address");
}

void TestSarosFixedMappingBeyondFoldedArena() {
	// Reproduced from build #645 on the device. Saros computes this guest
	// mapping *outside* the 512 GiB reservation rather than inside its top.
	// A larger folded image shifts both the reservation and the failed mapping
	// together, so the host image needs independent extra VA headroom.
	FoldPlan plan {};
	Check(PlanFold(512 * kGiB, k2MiB, kWindowStart, kWindowEnd, 48 * kGiB, &plan),
	      "Saros 48 GiB plan exists");
	constexpr uint64_t guest = 0x9cc0000000ull;
	constexpr uint64_t bytes = 192ull << 20;
	constexpr uint64_t host = 0x1cc0000000ull;
	uint64_t folded = 0;
	Check(FoldSpan(guest, bytes, &folded) && folded == host,
	      "Saros 192 MiB fixed map folds to observed host target");
	Check(guest - (plan.start + 512 * kGiB) == 3 * kGiB,
	      "Saros fixed direct map is three GiB beyond the 512 GiB arena");
	Check(folded >= plan.alias_end && folded + bytes <= plan.alias_end + 4 * kGiB,
	      "4 GiB host headroom covers the previously unavailable mapping");
	Check(plan.alias_end + 4 * kGiB < kWindowEnd,
	      "host folded headroom stays inside ARM64 address window");
}

void TestSarosMappedGuestOwner() {
	constexpr uint64_t high_guest = 0x9cc0000000ull;
	constexpr uint64_t low_guest  = 0x1cc0000000ull;
	constexpr uint64_t host       = 0x1cc0000000ull;
	static_assert(Fold(high_guest) == low_guest);
	Check(SameGuestAtHost(high_guest, host, high_guest, host, host),
	      "the exact high guest mapping can be renewed");
	Check(SameGuestAtHost(high_guest, host, high_guest + 0x4000, host + 0x4000, host + 0x4000),
	      "partial mapped-region splits retain original guest address");
	Check(!SameGuestAtHost(high_guest, host, low_guest, host, host),
	      "a low mapped guest must not silently own the high guest mapping");
	Check(!SameGuestAtHost(low_guest, host, high_guest, host, host),
	      "a high mapped guest must not silently overwrite a low guest mapping");
	Check(!SameGuestAtHost(high_guest, host, UINT64_MAX - 1, host, host + 0x4000),
	      "overflowing previous guest offsets never alias");
}

void TestSarosRelocatedHighHostBank() {
	constexpr uint64_t high_guest = 0x9cc0000000ull;
	constexpr uint64_t low_guest  = 0x1cc0000000ull;
	constexpr uint64_t shift = 256ull * kGiB;
	static_assert(Fold(high_guest) == low_guest, "constant evaluation keeps the baseline");
	Check(HostShift() == 0, "other titles use zero host shift");
	SetHostShift(shift);
	uint64_t high_host = 0;
	uint64_t low_host = 0;
	Check(FoldSpan(high_guest, 192ull * 1024 * 1024, &high_host),
	      "Saros 192 MiB map remains a contiguous high-bank host span");
	Check(FoldSpan(low_guest, 0x10000, &low_host), "Saros 64 KiB low span remains valid");
	Check(high_host == 0x5cc0000000ull && low_host == low_guest && high_host != low_host,
	      "the two observed Saros guest mappings must never share host addresses");
	Check(high_host + 192ull * 1024 * 1024 <= kWindowEnd,
	      "relocated guest range remains inside the 39-bit host user arena");
	Check(Fold(0x8000000000ull) == shift, "first upper-bank guest page is relocated");
	Check(Fold(0x1cc0000000ull) == low_guest, "lower bank preserves guest identity");
	SetHostShift(0);
	Check(Fold(high_guest) == low_guest, "non-Saros folding remains unchanged");
}

void TestAprReadBackingRange() {
	constexpr uint64_t limit = 0x7000000000ull;
	uint64_t host = 0;
	Check(Libs::AprReadRange::Resolve(0xcc001a6000ull, 194866, limit, true, &host) &&
	          host == 0x4c001a6000ull, "MFGT APR index buffer uses folded backing");
	Check(!Libs::AprReadRange::Resolve(0xcc001a6000ull, 194866, limit, false, &host),
	      "original ARM host-limit comparison rejects the guest address");
	Check(Libs::AprReadRange::Resolve(0xd400000000ull, 0x1000, limit, true, &host) &&
	          host == 0x5400000000ull, "MFGT third arena is valid for APR reads");
	Check(Libs::AprReadRange::Resolve(limit - 8, 8, limit, true, &host), "APR exact host end");
	for (const auto guest : {kFoldSize - 4, kDomainEnd - 4, UINT64_MAX - 4})
		Check(!Libs::AprReadRange::Resolve(guest, 8, limit, true, &host),
		      "APR rejects split or overflowing spans");
	Check(!Libs::AprReadRange::Resolve(limit - 8, 9, limit, true, &host), "APR host overrun");
	Check(!Libs::AprReadRange::Resolve(0xf000000000ull, 8, limit, true, &host),
	      "folding does not admit backing above host limit");
	Check(!Libs::AprReadRange::Resolve(0x1000, 0, limit, true, &host) &&
	          !Libs::AprReadRange::Resolve(0x1000, 0x100000001ull, limit, true, &host),
	      "APR zero and oversized reads remain invalid");
	Check(Libs::AprReadRange::Resolve(0xcc001a6000ull, 194866, 0xf00000000000ull, false, &host) &&
	          host == 0xcc001a6000ull, "x86 APR retains guest addresses unchanged");
}

void TestSmallerRequestsAndRejections() {
	FoldPlan plan {};
	// 400 GiB: the folded image is smaller, so the alias is the requested size and below the range.
	Check(PlanFold(400 * kGiB, k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &plan), "400 GiB plan");
	Check(plan.alias_end - plan.alias_start == 32 * kGiB && plan.alias_end <= plan.start,
	      "400 GiB alias");
	Check(plan.start + 400 * kGiB > kFoldSize, "400 GiB plan reaches the domain");
	Check(!PlanFold(513 * kGiB, k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &plan),
	      "larger than the fold size");
	Check(!PlanFold(0, k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &plan), "empty");
	Check(!PlanFold(512 * kGiB, 3 * k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &plan),
	      "bad alignment");
	Check(!PlanFold(512 * kGiB, k2MiB, kWindowStart, kWindowEnd, 0, &plan), "no alias");
	Check(!PlanFold(512 * kGiB, k2MiB, kWindowStart, kWindowStart + 16 * kGiB, 32 * kGiB, &plan),
	      "start beyond the window");
	// A small range would have to start above the window to reach 2^39: no plan.
	Check(!PlanFold(64 * kGiB, k2MiB, kWindowStart, kWindowEnd, 32 * kGiB, &plan),
	      "small ranges get no fold plan");
}

void TestHostPlanMatchesLayout() {
#if KYTY_HOST_ARM64
	FoldPlan plan {};
	Check(PlanHostFold(512 * kGiB, k2MiB, &plan) && plan.start == 0x1800000000ull,
	      "host plan on ARM64");
#endif
}

} // namespace

int main() {
	TestFoldIsIdentityBelowTheBoundary();
	TestFoldMovesTheDomain();
	TestFoldSpan();
	TestAprReadBackingRange();
	TestUnrealPlan();
	TestSarosObservedFoldedDirectMapping();
	TestSarosFixedMappingBeyondFoldedArena();
	TestSarosMappedGuestOwner();
	TestSarosRelocatedHighHostBank();
	TestSmallerRequestsAndRejections();
	TestHostPlanMatchesLayout();
	std::printf("AddressFoldTests: all passed\n");
	return 0;
}
