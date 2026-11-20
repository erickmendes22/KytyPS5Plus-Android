#include "graphics/host_gpu/renderer/pipeline/demonsLightingMemoryPolicy.h"

using Libs::Graphics::DemonsLightingMemoryPolicy::NeedsCoherentScalarReads;
using Libs::Graphics::DemonsLightingMemoryPolicy::NeedsStableSampledImages;
using Libs::Graphics::DemonsLightingMemoryPolicy::NeedsBvhProducerBarrier;

static_assert(NeedsCoherentScalarReads(0xc2e01fbe98f5ebabull));
static_assert(NeedsCoherentScalarReads(0x23056085c1120461ull));
static_assert(NeedsCoherentScalarReads(0x3dfe402653925fbfull));
static_assert(NeedsCoherentScalarReads(0xfabe0bcb275a3005ull));
static_assert(NeedsCoherentScalarReads(0xe75589aa0bb3ae52ull));
static_assert(NeedsCoherentScalarReads(0x93b0be7f1a0293d1ull));
static_assert(NeedsCoherentScalarReads(0x3265b01ed00a5c01ull));
static_assert(!NeedsCoherentScalarReads(0ull));
static_assert(!NeedsCoherentScalarReads(0x123456789abcdef0ull));

static_assert(NeedsStableSampledImages(0x12345ull, true, false, 1u));
static_assert(NeedsStableSampledImages(0x12345ull, true, false, 4u));
static_assert(NeedsStableSampledImages(0x12345ull, false, true, 2u));
static_assert(!NeedsStableSampledImages(0x12345ull, false, true, 1u));
static_assert(!NeedsStableSampledImages(0x12345ull, false, false, 4u));
static_assert(NeedsBvhProducerBarrier(true, true));
static_assert(!NeedsBvhProducerBarrier(false, true));
static_assert(!NeedsBvhProducerBarrier(true, false));
int main() { return 0; }
