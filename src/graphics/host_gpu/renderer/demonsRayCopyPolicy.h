#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace Libs::Graphics::DemonsRayCopyPolicy {
// 684e797dce67bb8e (bb6f031 SPIR-V): global thread id < count,
// (0xffffff < map[base + thread]), then copy 256 pairs from that
// source row to the indexed destination. The two 32-bit multiplies
// (<<8, then *8) wrap: flags are not part of the byte address.
// No BVH, sampled images or DPP.
constexpr uint32_t RaysPerProbe = 256;
constexpr uint32_t BytesPerProbe = RaysPerProbe * 8;
constexpr uint32_t NoRow = UINT32_MAX;
constexpr uint32_t ProbeCount = 92160;
constexpr uint32_t Destination(uint32_t raw) {
    if (raw <= 0x00ffffffu) return NoRow;
    const uint32_t byte_offset = raw * BytesPerProbe;
    const uint32_t probe = byte_offset / BytesPerProbe;
    return probe < ProbeCount ? probe : NoRow;
}
constexpr uint32_t ActiveRows(uint32_t base, uint32_t count,
                              uint32_t map_records, uint32_t groups) {
    if (base >= map_records) return 0;
    return static_cast<uint32_t>(std::min<uint64_t>(
        std::min<uint32_t>(count, map_records - base), uint64_t{groups} * 64));
}
struct Match { uint32_t row = NoRow; uint32_t matches = 0; };
constexpr Match Find(std::span<const uint32_t> map, uint32_t base,
                     uint32_t count, uint32_t probe) {
    Match result;
    if (probe >= ProbeCount || base >= map.size()) return result;
    const auto end = std::min<uint64_t>(map.size(), uint64_t{base} + count);
    for (uint32_t row = base; row < end; ++row) {
        if (Destination(map[row]) == probe) { result.row = row; ++result.matches; }
    }
    return result;
}
} // namespace Libs::Graphics::DemonsRayCopyPolicy
