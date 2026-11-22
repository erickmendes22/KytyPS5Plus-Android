#include "graphics/host_gpu/renderer/demonsDepthFullCoveragePolicy.h"
#include <array>
#include <cassert>
using Libs::Graphics::CompareDepthFull;
static_assert(!Libs::Graphics::HasDepth(0u));
static_assert(Libs::Graphics::HasDepth(0x3f800000u));
static_assert(Libs::Graphics::HasDepth(0x3e800000u));
static_assert(!Libs::Graphics::HasDepth(0x7fc00000u));
int main() {
    std::array<uint32_t, 4> before {0u, 0x3f800000u, 0x3e800000u, 0u};
    std::array<uint32_t, 4> after {0u, 0u, 0x3e800000u, 0x3f800000u};
    const auto d = CompareDepthFull(before, after, 2, 2);
    assert(d.valid && d.changed == 2);
    assert(d.filled_before == 2 && d.filled_after == 2);
    assert(d.far_before == 2 && d.far_after == 2);
    assert(d.min_x == 1 && d.max_x == 1 && d.min_y == 0 && d.max_y == 1);
    assert(!CompareDepthFull(before, after, 100, 100).valid);
}
