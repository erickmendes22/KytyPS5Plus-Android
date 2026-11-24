#pragma once
#include <cstdint>

namespace Libs::Graphics::IndirectDrawPacketPolicy {
// PM4 DRAW_(INDEX_)INDIRECT_MULTI word 4 bit 31 enables the subdraw-index
// SGPR destination in its low half. Same semantics as the PC renderer.
// An absent destination must not write SH_NOP or change GPU fast paths.
constexpr uint32_t NoRegister = 0x280u;
constexpr uint32_t SubdrawRegister(uint32_t control) {
    return (control & 0x80000000u) ? (control & 0xffffu) : NoRegister;
}
constexpr bool RequiresCpu(uint32_t first_index, uint32_t draw_index) {
    return first_index != NoRegister || draw_index != NoRegister;
}
}
