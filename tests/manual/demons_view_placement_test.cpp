#include "graphics/host_gpu/renderer/image/viewPlacementPolicy.h"
#include <cstdint>
using Libs::Graphics::ViewPlacementPolicy::Fits;

// A distinct guest G-buffer placed at native array slice 1 must not be
// sampled as slice 0. Descriptor BASE_ARRAY stays relative to that slice.
static_assert(Fits(0, 1, 0, 0, 1, 1, 1, 4));
static_assert(Fits(0, 1, 0, 2, 1, 1, 1, 4));
static_assert(!Fits(0, 1, 0, 3, 1, 1, 1, 4));

// A guest address outside the native image has no proven placement, and
// therefore must receive an independent Vulkan image.
static_assert(!Fits(-1, -1, 0, 0, 1, 1, 1, 1));
static_assert(!Fits(0, -1, 0, 0, 1, 1, 1, 1));
static_assert(!Fits(0, 0, 0, 0, 2, 1, 1, 1));

// Malformed PS5 descriptors cannot wrap the first level/layer arithmetic.
static_assert(!Fits(INT32_MAX, INT32_MAX, UINT32_MAX, UINT32_MAX,
                    UINT32_MAX, UINT32_MAX, 2, 2));
using Libs::Graphics::ViewPlacementPolicy::Surface;
using Libs::Graphics::ViewPlacementPolicy::KeepDistinct;

// Log frame 280: distinct 1600x900 G-buffer source T#s, incorrectly
// resolved to the same VkImage at 0x282b60000 for six compute shaders.
constexpr Surface a {0x2831e0000ull, 0x6c0000ull, 1600, 900, 1, 1, 1, 130, 2, 1, 4};
constexpr Surface b {0x282b60000ull, 0x6c0000ull, 1600, 900, 1, 1, 1, 130, 2, 1, 4};
static_assert(KeepDistinct(a, b));
static_assert(!KeepDistinct(a, a));
static_assert(!KeepDistinct(Surface{a.address, a.size, a.width, a.height, 1,
    1, 2, a.format, a.type, a.samples, a.tile}, b));
static_assert(!KeepDistinct(Surface{a.address, a.size, a.width, a.height, 1,
    2, 1, a.format, a.type, a.samples, a.tile}, b));

int main() { return 0; }
