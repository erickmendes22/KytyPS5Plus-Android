#pragma once

namespace Libs::Graphics::ImageInitPolicy {

// Ported from chenxiao07/KytyPS5: freshly allocated Vulkan image memory is
// undefined, whereas a newly allocated PS5 guest surface starts at zero.
// Do not touch compressed blocks or images with existing subresource state.
constexpr bool ShouldZeroOnFirstUse(bool is_undefined, bool no_subresource_states,
                                    bool has_image, bool uncompressed_format,
                                    bool enabled) noexcept {
    return enabled && is_undefined && no_subresource_states &&
           has_image && uncompressed_format;
}

} // namespace Libs::Graphics::ImageInitPolicy
