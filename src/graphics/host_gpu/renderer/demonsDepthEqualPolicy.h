#pragma once

#include <cstdint>

namespace Libs::Graphics::DemonsDepthEqualPolicy {

// PS5 depth EQUAL must be preserved unless an explicitly scoped compatibility
// policy is selected. In the 769 gameplay capture, these two material programs
// rasterized 1,749 and 96 fragments yet both had 0 depth-passing fragments,
// despite the D32 prepass being populated. The trace below also measures both
// relaxed comparisons against the SAME depth surface; no global depth change.
enum class Decision : uint8_t { Guest = 0, LessOrEqual = 1, GreaterOrEqual = 2 };

constexpr uint64_t DepthAddress = 0x282b60000ull;
constexpr uint64_t SuspectScene0 = 0xcfc1d9193e7f9e53ull;
constexpr uint64_t SuspectScene1 = 0xc92deb23fc611b10ull;

constexpr bool Material(uint64_t ps) {
    return ps == 0xafd9334e8581cda2ull || ps == 0xe31a5473e9ee7503ull ||
           ps == 0xb631777eb4aee68cull || ps == SuspectScene0 ||
           ps == SuspectScene1;
}

// mode 0: guest comparison (the 769-derived AUTO relaxation did not help on device)
// mode 1: explicit LESS_OR_EQUAL for all five material shaders
// mode 2: explicit GREATER_OR_EQUAL for all five
// mode 3: exact guest EQUAL, for A/B recovery in the launcher.
constexpr Decision Select(int mode, uint64_t ps, uint64_t depth_address,
                          bool depth_test, bool depth_write, bool stencil_always,
                          uint32_t guest_compare) {
    if (!Material(ps) || depth_address != DepthAddress || !depth_test ||
        depth_write || !stencil_always || guest_compare != 2u) return Decision::Guest;
    if (mode == 1) return Decision::LessOrEqual;
    if (mode == 2) return Decision::GreaterOrEqual;
    return Decision::Guest;
}

} // namespace Libs::Graphics::DemonsDepthEqualPolicy
