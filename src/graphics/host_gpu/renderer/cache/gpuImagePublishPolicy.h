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

// A sampled read must not replace a native image containing newer GPU writes
// with a stale, clean image of the same guest surface. The previous caller
// only compared candidates when BOTH were GPU-modified, so an older unmodified
// duplicate silently won when encountered later in the page-table scan.
//
// CPU writes are authoritative and must first be reconciled by the normal
// dirty-page/refresh path: never choose between aliases in that case.
[[nodiscard]] constexpr Decision ChooseReadOwner(bool first_gpu_written,
                                                 bool next_gpu_written,
                                                 uint64_t first_write,
                                                 uint64_t next_write,
                                                 bool exact_same_layout,
                                                 bool cpu_dirty) noexcept {
    if (!exact_same_layout || cpu_dirty) return Decision::Refuse;
    if (first_gpu_written != next_gpu_written)
        return first_gpu_written ? Decision::Keep : Decision::Replace;
    if (first_gpu_written) return ChooseAlias(first_write, next_write, true);
    return Decision::Refuse;
}
static_assert(ChooseReadOwner(true, false, 17, 0, true, false) == Decision::Keep);
static_assert(ChooseReadOwner(false, true, 0, 18, true, false) == Decision::Replace);
static_assert(ChooseReadOwner(true, true, 17, 18, true, false) == Decision::Replace);
static_assert(ChooseReadOwner(true, true, 18, 17, true, false) == Decision::Keep);
static_assert(ChooseReadOwner(true, false, 17, 0, true, true) == Decision::Refuse);
static_assert(ChooseReadOwner(true, false, 17, 0, false, false) == Decision::Refuse);
} // namespace Libs::Graphics::GpuImagePublishPolicy
