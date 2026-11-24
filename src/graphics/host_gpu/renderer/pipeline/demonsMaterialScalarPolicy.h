#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace Libs::Graphics::DemonsMaterialScalarPolicy {
// Original shader 552d823dcfcf2dee, decoded as GFX10:
// pc 0x008: s_load_dwordx2 vcc, s[0:1], 0x8
// pc 0x0c0: s_load_dwordx2 vcc, vcc, 0x2a8
// pc 0x170: s_load_dwordx2 vcc, s[0:1], 0x18
// pc 0x184: s_load_dwordx4 s[0:3], vcc, 0x10
// pc 0x1a0: s_load_dword vcc_lo, vcc, 0x20
constexpr uint64_t Shader = 0x552d823dcfcf2deeull;
constexpr uint64_t AddressMask = 0x0000ffffffffffffull;
constexpr uint32_t NoSlot = UINT32_MAX;
constexpr uint64_t Pointer(uint32_t low, uint32_t high) {
    return ((uint64_t{high} << 32) | low) & AddressMask & ~uint64_t{3};
}
constexpr uint32_t Field(uint32_t offset) {
    switch (offset) {
    case 0x10: return 0;
    case 0x14: return 1;
    case 0x18: return 2;
    case 0x20: return 4;
    default: return NoSlot;
    }
}
constexpr bool Layout(std::span<const uint32_t> flat,
                      const std::array<uint32_t,2>& first,
                      const std::array<uint32_t,2>& second,
                      const std::array<uint32_t,2>& screen) {
    return flat.size() == 10 && flat[0] == first[0] && flat[1] == first[1] &&
        flat[2] == second[0] && flat[3] == second[1] &&
        flat[4] == screen[0] && flat[5] == screen[1];
}
constexpr bool Fields(const std::array<uint32_t,4>& fields) {
    uint32_t mask = 0;
    for (auto f: fields) {
        if (f != 0 && f != 1 && f != 2 && f != 4) return false;
        mask |= 1u << f;
    }
    return mask == 0x17;
}
} // namespace Libs::Graphics::DemonsMaterialScalarPolicy
