#pragma once

#include <cstdint>

// PS5 VideoOutOutputStatus::resolution values used by Kyty: 1 = HD output,
// 2 = UHD output. Output size in the Android UI is not the same thing as this
// guest-facing status: older emulation code advertised UHD even at 720p unless
// the game's param.json had ATTRIBUTE3 bit 2 set.
//
// A title-scoped, opt-in correction lets UFC 6 choose its HD-mode Frostbite
// render preset when the user actually selected a <=1080p output. It is not
// a global fake resolution and does not resize VkImages after creation.
namespace Libs::VideoOut::OutputResolutionPolicy {

[[nodiscard]] constexpr uint32_t Select(uint32_t width, uint32_t height,
                                        bool attribute_allows_detection,
                                        bool title_requests_hd_status) noexcept {
    const bool output_is_hd = width > 0 && height > 0 &&
                              width <= 1920 && height <= 1080;
    return output_is_hd && (attribute_allows_detection || title_requests_hd_status)
               ? 1u : 2u;
}

} // namespace Libs::VideoOut::OutputResolutionPolicy
