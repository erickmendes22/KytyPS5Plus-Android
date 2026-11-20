#pragma once
#include <cstdint>

namespace Libs::Graphics::ComputeBufferFillPolicy {
// The uniform fill's resource, stride, bounds and non-aliasing proof is
// checked before this predicate. Preserve the guest dispatch whenever an
// image overlaps, notably UFC6's live storage/depth-stencil alias.
[[nodiscard]] constexpr bool UseUncachedBufferFill(uint64_t address, uint64_t size,
                                                  bool image_overlap) noexcept {
    return !image_overlap && address != 0 && size != 0 &&
           (address & 3u) == 0 && (size & 3u) == 0;
}
} // namespace Libs::Graphics::ComputeBufferFillPolicy
