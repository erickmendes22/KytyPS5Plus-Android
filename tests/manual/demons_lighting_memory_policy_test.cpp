#include "graphics/shader/demonsPcDppPolicy.h"
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

static_assert(Libs::Graphics::DemonsPcDppPolicy::Applies(true, Libs::Graphics::DemonsPcDppPolicy::RayProducer));
static_assert(Libs::Graphics::DemonsPcDppPolicy::Applies(true, Libs::Graphics::DemonsPcDppPolicy::Relight));
static_assert(!Libs::Graphics::DemonsPcDppPolicy::Applies(false, Libs::Graphics::DemonsPcDppPolicy::Relight));
static_assert(!Libs::Graphics::DemonsPcDppPolicy::Applies(true, 0x1234));
static_assert(!Libs::Graphics::DemonsPcDppPolicy::CaptureProducer(false, Libs::Graphics::DemonsPcDppPolicy::RayProducer));
static_assert(!Libs::Graphics::DemonsPcDppPolicy::CaptureProducer(true, Libs::Graphics::DemonsPcDppPolicy::Relight));
static_assert(Libs::Graphics::DemonsPcDppPolicy::CaptureProducer(true, Libs::Graphics::DemonsPcDppPolicy::RayProducer));
static_assert(Libs::Graphics::DemonsPcDppPolicy::CaptureActive(10, false));
static_assert(!Libs::Graphics::DemonsPcDppPolicy::CaptureActive(17, false));
static_assert(Libs::Graphics::DemonsPcDppPolicy::CaptureActive(17, true));

#include "graphics/host_gpu/renderer/demonsRayCopyPolicy.h"
namespace {
using namespace Libs::Graphics::DemonsRayCopyPolicy;
constexpr uint32_t map_fixture[] = {4445, 4445, 12448, 4445, 0xffffffu, 4445};
static_assert(ActiveRows(2, 11863, 11865, 186) == 11863);
static_assert(ActiveRows(2, 10353, 10355, 162) == 10353);
static_assert(ActiveRows(2, 100, 6, 1) == 4);
static_assert(ActiveRows(7, 10, 6, 1) == 0);
static_assert(Find(map_fixture, 2, 3, 4445).row == 3);
static_assert(Find(map_fixture, 2, 3, 4445).matches == 1);
static_assert(Find(map_fixture, 2, 4, 4445).matches == 2);
static_assert(Find(map_fixture, 2, 3, 0xffffffu).matches == 0);
static_assert(Find(map_fixture, 2, 3, 902).matches == 0);
}
