#include "graphics/host_gpu/renderer/pipeline/sampledDepthMetadataPolicy.h"

#include <cassert>
#include <cstdint>

using Libs::Graphics::IsSupportedDepthMetadataControl;

// The actual offending Demon descriptor has WORD6 == 0x00000003:
// the low byte is a mip-stats counter and compression is disabled.
static_assert(IsSupportedDepthMetadataControl(0x00000003u, false, false));
static_assert(IsSupportedDepthMetadataControl(0x00000000u, false, false));
static_assert(IsSupportedDepthMetadataControl(0x00080003u, false, false));

// Compressed depth still requires properly encoded HTILE controls; the low
// eight counter bits are ignored and the rest remain strictly validated.
static_assert(IsSupportedDepthMetadataControl(0x00280003u, true, false));
static_assert(IsSupportedDepthMetadataControl(0x002804ffu, true, true));
static_assert(!IsSupportedDepthMetadataControl(0x00000003u, true, false));
static_assert(!IsSupportedDepthMetadataControl(0x00200003u, true, false));
static_assert(!IsSupportedDepthMetadataControl(0x002804ffu, true, false));

int main() {
    assert(IsSupportedDepthMetadataControl(3u, false, false));
    assert(!IsSupportedDepthMetadataControl(3u, true, false));
    return 0;
}
