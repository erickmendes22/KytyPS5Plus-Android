#include "graphics/host_gpu/renderer/sampleHtileClearPolicy.h"
#include <cassert>
using Libs::Graphics::DecideSampleHtile;
static_assert(DecideSampleHtile(true, true, 0x00000000u, false).clear_depth);
static_assert(!DecideSampleHtile(true, true, 0x0000000fu, true).clear_depth);
static_assert(!DecideSampleHtile(false, true, 0u, true).clear_depth);
static_assert(!DecideSampleHtile(true, false, 0u, true).clear_depth);
static_assert(DecideSampleHtile(true, true, 0x00000000u, true).keep_stencil_meta);
static_assert(!DecideSampleHtile(true, true, 0x00000300u, true).keep_stencil_meta);
int main(){const auto a=DecideSampleHtile(true,true,0u,true);assert(a.clear_depth && a.keep_stencil_meta);}
