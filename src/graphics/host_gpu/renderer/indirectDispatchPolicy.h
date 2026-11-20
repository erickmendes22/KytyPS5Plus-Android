#pragma once

#include <cstdint>

namespace Libs::Graphics {

// A zero observed in guest RAM is not necessarily a zero in a GPU-owned
// dispatch-indirect buffer. For a title that produces the lighting group's
// arguments on the GPU, keep vkCmdDispatchIndirect as the source of truth.
// This policy never manufactures workgroup dimensions.
enum class IndirectDispatchSource : uint8_t { Skip, Cpu, Gpu };

// For a title whose dispatch group counts are produced by the GPU, it is
// unsafe to shrink StorageBuffer-backed LDS from a CPU snapshot. The guest
// memory tracker may already have published the arguments to an ordinary
// Vulkan buffer and cleared its dirty flag, even though guest RAM still
// contains an older zero. Bind the existing full utility-buffer capacity;
// vkCmdDispatchIndirect remains the only source of the actual group counts.
// This changes a descriptor bound, not the amount of device memory allocated.
[[nodiscard]] constexpr bool ReserveIndirectLdsFromGpu(uint64_t args_address,
                                                        bool gpu_group_policy) noexcept {
    return gpu_group_policy && args_address != 0;
}


[[nodiscard]] constexpr IndirectDispatchSource ChooseIndirectDispatchSource(
    bool host_args_current, bool host_has_zero_group,
    bool gpu_authoritative_zero) noexcept {
    if (!host_args_current) return IndirectDispatchSource::Gpu;
    if (!host_has_zero_group) return IndirectDispatchSource::Cpu;
    return gpu_authoritative_zero ? IndirectDispatchSource::Gpu
                                  : IndirectDispatchSource::Skip;
}

} // namespace Libs::Graphics
