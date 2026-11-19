#pragma once

#include <cstdint>

namespace Libs::Graphics {

// A zero observed in guest RAM is not necessarily a zero in a GPU-owned
// dispatch-indirect buffer. For a title that produces the lighting group's
// arguments on the GPU, keep vkCmdDispatchIndirect as the source of truth.
// This policy never manufactures workgroup dimensions.
enum class IndirectDispatchSource : uint8_t { Skip, Cpu, Gpu };

[[nodiscard]] constexpr IndirectDispatchSource ChooseIndirectDispatchSource(
    bool host_args_current, bool host_has_zero_group,
    bool gpu_authoritative_zero) noexcept {
    if (!host_args_current) return IndirectDispatchSource::Gpu;
    if (!host_has_zero_group) return IndirectDispatchSource::Cpu;
    return gpu_authoritative_zero ? IndirectDispatchSource::Gpu
                                  : IndirectDispatchSource::Skip;
}

} // namespace Libs::Graphics
