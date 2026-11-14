#include "graphics/host_gpu/renderer/image/zeroNewImagePolicy.h"

#include <cassert>
using Libs::Graphics::ImageInitPolicy::ShouldZeroOnFirstUse;

static_assert(ShouldZeroOnFirstUse(true, true, true, true, true));
static_assert(!ShouldZeroOnFirstUse(false, true, true, true, true));
static_assert(!ShouldZeroOnFirstUse(true, false, true, true, true));
static_assert(!ShouldZeroOnFirstUse(true, true, false, true, true));
static_assert(!ShouldZeroOnFirstUse(true, true, true, false, true));
static_assert(!ShouldZeroOnFirstUse(true, true, true, true, false));

int main() {
    // Newly allocated uncompressed PS5 render/depth attachments are initialized.
    assert(ShouldZeroOnFirstUse(true, true, true, true, true));
    // Existing content, layered tracking and compressed formats are untouched.
    assert(!ShouldZeroOnFirstUse(false, true, true, true, true));
    assert(!ShouldZeroOnFirstUse(true, false, true, true, true));
    assert(!ShouldZeroOnFirstUse(true, true, true, false, true));
    return 0;
}
