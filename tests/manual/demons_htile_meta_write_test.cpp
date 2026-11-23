#include "graphics/host_gpu/renderer/htileMetaWritePolicy.h"
#include <cassert>
using Libs::Graphics::HtileMetaWritePolicy::MustInvalidatePendingClear;
static_assert(MustInvalidatePendingClear(true,true,false));
static_assert(!MustInvalidatePendingClear(true,true,true));
static_assert(!MustInvalidatePendingClear(true,false,false));
static_assert(!MustInvalidatePendingClear(false,true,false));
int main() {assert(MustInvalidatePendingClear(true,true,false));}
