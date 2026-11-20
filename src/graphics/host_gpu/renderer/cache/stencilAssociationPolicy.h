#pragma once

namespace Libs::Graphics {

// A guest allocation can alias an image's stencil backing without being a
// stencil record. Never reparent live GPU targets as stencil bookkeeping.
// Matches the real Demon's Souls repair in chenxiao07/KytyPS5 24a7809221f.
[[nodiscard]] constexpr bool ReusableAsStencilRecord(bool color_target,
                                                       bool storage,
                                                       bool video_out,
                                                       bool depth_target) noexcept {
    return !color_target && !storage && !video_out && !depth_target;
}

// A stencil bookkeeping image that was attached to another depth owner may
// not be stolen for this depth target, even when the plane's guest address
// and dimensions coincide. Guest depth images can alternate every frame;
// retaining each owner's own record prevents the second reparent from
// invalidating a stencil view still needed by the first depth attachment.
[[nodiscard]] constexpr bool CanUseStencilForOwner(bool has_depth_parent,
                                                   bool parent_is_this_depth) noexcept {
    return !has_depth_parent || parent_is_this_depth;
}

} // namespace Libs::Graphics
