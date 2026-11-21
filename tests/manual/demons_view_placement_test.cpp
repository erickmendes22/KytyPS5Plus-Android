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
using Libs::Graphics::ViewPlacementPolicy::KeepDistinctDepth;
using Libs::Graphics::ViewPlacementPolicy::KeepDistinctSampledDepth;

// Log frame 280: distinct 1600x900 G-buffer source T#s, incorrectly
// resolved to the same VkImage at 0x282b60000 for six compute shaders.
constexpr Surface a {0x2831e0000ull, 0x6c0000ull, 1600, 900, 1, 1, 1, 130, 2, 1, 4};
constexpr Surface b {0x282b60000ull, 0x6c0000ull, 1600, 900, 1, 1, 1, 130, 2, 1, 4};
static_assert(KeepDistinct(a, b));
static_assert(!KeepDistinct(a, a));
// A guest S8 plane and its depth owner can have different base addresses;
// they are deliberately views of the same depth/stencil Vulkan allocation.
static_assert(!KeepDistinct(
    Surface{a.address, a.size, a.width, a.height, a.depth, 1, 1,
            a.format, a.type, a.samples, a.tile, true}, b));
static_assert(!KeepDistinct(a,
    Surface{b.address, b.size, b.width, b.height, b.depth, 1, 1,
            b.format, b.type, b.samples, b.tile, true}));

// Frame-280 real input was a full depth texture at 0x2831e0000
// resolved incorrectly to the equally shaped depth target at
// 0x282b60000. Both are D32/S8, but both represent depth, not a
// request for the S8 plane of one owner. Do not merge these images.
constexpr Surface depth_a {a.address, a.size, a.width, a.height, a.depth,
                           1, 1, a.format, a.type, a.samples, a.tile, true};
constexpr Surface depth_b {b.address, b.size, b.width, b.height, b.depth,
                           1, 1, b.format, b.type, b.samples, b.tile, true};
static_assert(KeepDistinctDepth(depth_a, depth_b, true));
// The actual G-buffer depth owner may include a different padding/metadata
// span. Identical full-depth geometry still cannot be the same VkImage.
constexpr Surface padded_depth_b {b.address, b.size + 0x10000u, b.width,
    b.height, b.depth, 1, 1, b.format, b.type, b.samples, b.tile, true};
static_assert(KeepDistinctDepth(depth_a, padded_depth_b, true));
static_assert(!KeepDistinctDepth(depth_a, depth_b, false)); // S8 association
static_assert(!KeepDistinctDepth(depth_a, depth_a, true));
static_assert(!KeepDistinctDepth(a, depth_b, true)); // color vs depth

// Gameplay: the first sampled D32 component is encoded as a single R32
// view, rather than a VkFormat depth attachment. Both full surfaces have
// different guest bases but the older overlap path reused depth_b.
constexpr Surface raw_depth_a {a.address, a.size, a.width, a.height, a.depth,
                               1, 1, 100, a.type, a.samples, a.tile, false};
static_assert(KeepDistinctSampledDepth(raw_depth_a, depth_b));
// Some shaders bind the same depth component as native D32_SFLOAT (126)
// rather than R32_SFLOAT (100), while the attachment stays D32/S8 (130).
constexpr Surface sampled_d32_a {a.address, a.size, a.width, a.height, a.depth,
                                 1, 1, 126, a.type, a.samples, a.tile, true};
static_assert(KeepDistinctSampledDepth(sampled_d32_a, depth_b));
static_assert(!KeepDistinctSampledDepth(
    Surface{depth_b.address, a.size, a.width, a.height, a.depth,
            1, 1, 100, a.type, a.samples, a.tile, false}, depth_b));
static_assert(!KeepDistinctSampledDepth(
    Surface{a.address, a.size, a.width + 1u, a.height, a.depth,
            1, 1, 100, a.type, a.samples, a.tile, false}, depth_b));
static_assert(!KeepDistinctSampledDepth(
    Surface{a.address, a.size, a.width, a.height, a.depth,
            1, 2, 100, a.type, a.samples, a.tile, false}, depth_b));
static_assert(!KeepDistinctSampledDepth(raw_depth_a, b));

static_assert(!KeepDistinctDepth(depth_a,
    Surface{b.address, b.size, b.width, b.height, 1, 1, 2,
            b.format, b.type, b.samples, b.tile, true}, true));

static_assert(!KeepDistinct(Surface{a.address, a.size, a.width, a.height, 1,
    1, 2, a.format, a.type, a.samples, a.tile}, b));
static_assert(!KeepDistinct(Surface{a.address, a.size, a.width, a.height, 1,
    2, 1, a.format, a.type, a.samples, a.tile}, b));

int main() { return 0; }
