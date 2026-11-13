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

} // namespace Libs::Graphics
