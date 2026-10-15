#include "common/hostArch.h"
#include "kernel/addressLayout.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Common::AddressLayout;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "AddressLayoutTests: failed: %s\n", message);
    std::abort();
  }
}

struct Range {
  const char *name;
  uint64_t start;
  uint64_t end; // exclusive
};

constexpr uint64_t kGiB = 1024ull * 1024 * 1024;

// Guest-mandated addresses that must keep their values on every host layout.
constexpr uint64_t kDefaultMapBase = 0x200000000ull;   // memory.cpp GUEST_DEFAULT_MAP_BASE
constexpr uint64_t kModuleBaseLow = 0x800000000ull;    // libKernel module region
constexpr uint64_t kModuleBaseHigh = 0x900000000ull;   // libKernel module region
constexpr uint64_t kDriverDmemBase = 0x0fe0000000ull;  // agc.cpp DriverDmemBase

constexpr Range HostSystem() {
  return {"host system", kHostSystemManagedMin, kHostSystemReservedMax + 1};
}
constexpr Range HostUser() { return {"host user", kHostUserMin, kHostUserEnd}; }
constexpr Range Extended() { return {"extended", kExtendedMemoryBase, kExtendedMemoryEnd}; }
constexpr uint64_t ShadowEnd() { return (kExtendedMemoryEnd >> 3u) + kAsanShadowOffset; }
constexpr Range Shadow() { return {"asan shadow", kAsanShadowOffset, ShadowEnd()}; }

bool Disjoint(const Range &a, const Range &b) {
  return a.end <= b.start || b.end <= a.start;
}

bool Contains(const Range &outer, const Range &inner) {
  return inner.start >= outer.start && inner.end <= outer.end;
}

void TestRangesAreInsideUsableTop() {
#if KYTY_HOST_ARM64
  constexpr uint64_t usable_top = kUsableTop;
  static_assert(usable_top == 0x7f00000000ull);
#else
  constexpr uint64_t usable_top = UINT64_MAX;
#endif
  const Range ranges[] = {HostSystem(), HostUser(), Extended(), Shadow(),
                          {"low arena", kLowArenaFloor, kLowArenaLimit},
                          {"prt aperture", kPrtApertureStart, kPrtApertureEnd}};
  for (const auto &range : ranges) {
    Check(range.start < range.end, "range is empty or inverted");
    Check(range.end <= usable_top, "range end is above the usable address-space top");
  }
}

void TestTopLevelRangesDoNotOverlap() {
  const Range top_level[] = {HostSystem(), HostUser(), Extended(), Shadow()};
  constexpr size_t count = sizeof(top_level) / sizeof(top_level[0]);
  for (size_t i = 0; i < count; i++) {
    for (size_t j = i + 1; j < count; j++) {
      Check(Disjoint(top_level[i], top_level[j]), "top-level address ranges overlap");
    }
  }
}

void TestNestedRangesStayInUserWindow() {
  const Range user = HostUser();
  Check(Contains(user, {"low arena", kLowArenaFloor, kLowArenaLimit}),
        "low arena leaves the user window");
  // The PRT aperture starts inside the system window and ends at the user window end.
  Check(kPrtApertureEnd == kHostUserEnd, "PRT aperture must end at the user window end");
  Check(kPrtApertureStart < kPrtApertureEnd, "PRT aperture is empty");
}

void TestGuestMandatedAddresses() {
  const Range system = HostSystem();
  for (const uint64_t address : {kDefaultMapBase, kModuleBaseLow, kModuleBaseHigh,
                                 kDriverDmemBase}) {
    Check(address >= system.start && address < system.end,
          "guest-mandated address left the host system window");
  }
  Check(kModuleBaseHigh + 0x100000000ull <= system.end,
        "module region extends past the host system window");
}

void TestExtendedRegionInsideTrackerWindow() {
  Check(kExtendedMemoryEnd <= kTrackerAddressSize,
        "extended memory is not inside the GPU page tracker window");
  Check(kLowerAddressSize <= kExtendedMemoryBase,
        "lower GPU window overlaps the extended memory");
  Check(kLowerAddressSize + kExtendedMemorySize <= kTrackerAddressSize,
        "packed GPU index space exceeds the tracker window");
  Check(kExtendedFoldOffset == kExtendedMemoryBase - kLowerAddressSize,
        "GPU fold offset is not derived from the window bounds");
#if KYTY_HOST_ARM64
  Check(kTrackerAddressBits == 39 && kTrackerAddressSize == (1ull << 39),
        "ARM64 tracker window is not 1<<39");
#endif
}

void TestReportedExtendedRangeMatchesLayout() {
  constexpr auto reported = ExtendedVirtualRange();
  static_assert(reported.start == kExtendedMemoryBase);
  static_assert(reported.end == kExtendedMemoryBase + kExtendedMemorySize);
  Check(reported.start == kExtendedMemoryBase && reported.end == kExtendedMemoryEnd,
        "reported extended range does not match the layout");
  Check(reported.end - reported.start == kExtendedMemorySize,
        "reported extended range size does not match the layout");
  Check(kMaxAppAddress >= reported.end,
        "guest application address limit is below the extended range end");
#if KYTY_HOST_ARM64
  Check(reported.end <= kUsableTop, "reported extended range exceeds the usable top");
  Check(kExtendedMemoryBase == 0x6000000000ull && kExtendedMemorySize == 64 * kGiB,
        "ARM64 extended memory is not 64 GiB at 0x6000000000");
  Check(kMaxAppAddress == reported.end, "ARM64 app address limit must equal extended end");
#endif
}

#if KYTY_HOST_X86
void TestX86ValuesUnchanged() {
  Check(kHostSystemManagedMin == 0x0000040000ull && kHostSystemManagedMax == 0x07ffffbfffull,
        "x86 system managed window changed");
  Check(kHostUserMax == 0xfbffffffffull, "x86 user window changed");
  Check(kExtendedMemoryBase == 0x080000000000ull && kExtendedMemorySize == 512 * kGiB,
        "x86 extended memory changed");
  Check(kTrackerAddressBits == 44 && kTrackerAddressSize == (1ull << 44),
        "x86 tracker window changed");
  Check(kLowerAddressSize == (1ull << 40), "x86 lower GPU window changed");
  Check(kLowArenaLimit == 0xfc00000000ull && kLowArenaFloor == 0xa000000000ull,
        "x86 arena bounds changed");
  Check(kPrtApertureStart == 0x0f00000000ull && kPrtApertureEnd == 0xfc00000000ull,
        "x86 PRT aperture changed");
  Check(kMaxAppAddress == 0x0000f00000000000ull, "x86 app address limit changed");
  Check(kAsanShadowOffset == 0x10000000000ull, "x86 ASAN shadow offset changed");
}
#endif

} // namespace

int main() {
  TestRangesAreInsideUsableTop();
  TestTopLevelRangesDoNotOverlap();
  TestNestedRangesStayInUserWindow();
  TestGuestMandatedAddresses();
  TestExtendedRegionInsideTrackerWindow();
  TestReportedExtendedRangeMatchesLayout();
#if KYTY_HOST_X86
  TestX86ValuesUnchanged();
#endif
  std::puts("AddressLayoutTests: all cases passed");
  return 0;
}
