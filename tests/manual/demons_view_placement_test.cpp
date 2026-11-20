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
int main() { return 0; }
