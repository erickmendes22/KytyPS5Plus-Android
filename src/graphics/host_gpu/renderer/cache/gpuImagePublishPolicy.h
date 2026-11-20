#pragma once
#include <cstdint>
namespace Libs::Graphics::GpuImagePublishPolicy {
// No guess from incomparable per-image versions or partial heap overlaps.
enum class Decision : uint8_t { Refuse, Keep, Replace };
[[nodiscard]] constexpr Decision ChooseAlias(uint64_t first_write, uint64_t next_write,
                                              bool exact_same_layout) noexcept {
    if (!exact_same_layout || first_write == 0 || next_write == 0 ||
        first_write == next_write) return Decision::Refuse;
    return next_write > first_write ? Decision::Replace : Decision::Keep;
}
} // namespace Libs::Graphics::GpuImagePublishPolicy
