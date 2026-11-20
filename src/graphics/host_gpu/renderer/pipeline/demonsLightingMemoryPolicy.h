#pragma once

#include <cstdint>

// Shader hashes are taken from the six Demon's Souls PPSA01341 lighting
// dispatches and the scene composite reported by the Android 50fb3da1 logs.
// Never turn on CPU/GPU synchronization in unrelated games or shader stages.
namespace Libs::Graphics::DemonsLightingMemoryPolicy {

[[nodiscard]] constexpr bool NeedsCoherentScalarReads(uint64_t hash) noexcept {
    switch (hash) {
        case 0xc2e01fbe98f5ebabull:
        case 0x23056085c1120461ull:
        case 0x3dfe402653925fbfull:
        case 0xfabe0bcb275a3005ull:
        case 0xe75589aa0bb3ae52ull:
        case 0x93b0be7f1a0293d1ull:
        case 0x3265b01ed00a5c01ull: return true;
        default: return false;
    }
}

} // namespace Libs::Graphics::DemonsLightingMemoryPolicy
