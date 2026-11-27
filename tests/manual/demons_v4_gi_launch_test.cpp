#include "graphics/shader/demonsV4LightTrace.h"
using namespace Libs::Graphics::DemonsV4LightTrace;
// Only X group ID is an input to the fingerprinted original q33 shader.
// No group-Y argument exists in the q33 guard.
static_assert(GiSupports(8,8,1,64,1,2,2,true,false));
static_assert(GiSupports(8,8,1,64,0,2,3,true,false));
static_assert(!GiSupports(8,8,1,64,1,2,2,false,false));
static_assert(!GiSupports(8,8,1,64,1,2,1,true,false));
static_assert(!GiSupports(8,8,1,32,1,2,2,true,false));
static_assert(!GiSupports(8,8,1,64,2,2,2,true,false));
static_assert(!GiSupports(8,8,1,64,1,2,2,true,true));
static_assert(!GiSupports(8,8,1,64,1,3,2,true,false));
static_assert(!GiSupports(64,1,1,64,1,2,2,true,false));
int main(){return 0;}
