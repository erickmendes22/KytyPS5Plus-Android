#pragma once
namespace Libs::Graphics::HtileMetaWritePolicy {
constexpr bool MustInvalidatePendingClear(bool demons_title, bool htile,
                                          bool proven_complete_uniform_fill) {
    return demons_title && htile && !proven_complete_uniform_fill;
}
} // namespace Libs::Graphics::HtileMetaWritePolicy
