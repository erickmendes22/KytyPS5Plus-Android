#include "graphics/host_gpu/renderer/htileLoadPolicy.h"
#include <cassert>
using Libs::Graphics::DecideHtileLoad;
static_assert(Libs::Graphics::HtileFillClearsDepth(0xfffc0000u));
static_assert(!Libs::Graphics::HtileFillClearsDepth(0xfffc000fu));
static_assert(!DecideHtileLoad(true, 0xfffc000fu, true, true).depth_meta_clear);
static_assert(DecideHtileLoad(true, 0xfffc000fu, true, true).stencil_meta_clear);
static_assert(!DecideHtileLoad(true, 0xfffc000fu, true, false).depth_meta_clear);
static_assert(DecideHtileLoad(true, 0xfffc0000u, true, true).depth_meta_clear);
static_assert(DecideHtileLoad(true, 0, false, true).depth_meta_clear);
static_assert(!DecideHtileLoad(false, 0xfffc000fu, true, true).stencil_meta_clear);
int main() {
    const auto decision = DecideHtileLoad(true, 0xfffc000fu, true, true);
    assert(!decision.depth_meta_clear && decision.stencil_meta_clear);
}
