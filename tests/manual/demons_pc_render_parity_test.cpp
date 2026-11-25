#include "graphics/shader/demonsPcRenderParity.h"
#include <cassert>
using namespace Libs::Graphics::DemonsPcRenderParity;
static_assert(PixelActive(true,0xf0,0xf0,false));
static_assert(!PixelActive(true,0xf0,0xf,false));
static_assert(PixelActive(true,0,0,true));
static_assert(!PixelActive(false,0xf,0xf,true));
static_assert(DppSkipSource(true,false,false,false));
static_assert(!DppSkipSource(true,true,false,false));
static_assert(!DppSkipSource(true,false,true,false));
static_assert(!DppSkipSource(true,false,false,true));
static_assert(MetaLayers(0x1000,0x400,4,0x1000,0x100,false,false).count==4);
static_assert(MetaLayers(0x1000,0x400,4,0x1100,0x100,true,false).first==1);
static_assert(MetaLayers(0x1000,0x400,4,0x1100,0x100,true,false).count==1);
static_assert(!MetaLayers(0x1000,0x400,4,0x1100,0x100,true,false).exact);
static_assert(MetaLayers(0x1000,0x400,4,0x1000,0x500,true,true).count==0);
int main(){assert(!DppSkipSource(false,false,false,false));}
