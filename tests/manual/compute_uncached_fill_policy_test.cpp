#include "graphics/host_gpu/renderer/computeBufferFillPolicy.h"
#include <cassert>
using Libs::Graphics::ComputeBufferFillPolicy::UseUncachedBufferFill;
static_assert(UseUncachedBufferFill(0x1000, 128, false));
static_assert(UseUncachedBufferFill(0x1000, 4, false));
static_assert(!UseUncachedBufferFill(0x1000, 128, true)); // UFC6: storage/stencil alias
static_assert(!UseUncachedBufferFill(0x1001, 128, false));
static_assert(!UseUncachedBufferFill(0x1000, 127, false));
static_assert(!UseUncachedBufferFill(0, 128, false));
static_assert(!UseUncachedBufferFill(0x1000, 0, false));
int main() {
    assert(UseUncachedBufferFill(0x1000, 128, false));
    assert(!UseUncachedBufferFill(0x1000, 128, true));
}
