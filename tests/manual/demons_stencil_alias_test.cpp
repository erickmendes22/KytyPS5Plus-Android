#include "graphics/host_gpu/renderer/cache/stencilAssociationPolicy.h"

#include <cassert>
using Libs::Graphics::ReusableAsStencilRecord;

static_assert(ReusableAsStencilRecord(false, false, false, false));
static_assert(!ReusableAsStencilRecord(true, false, false, false));
static_assert(!ReusableAsStencilRecord(false, true, false, false));
static_assert(!ReusableAsStencilRecord(false, false, true, false));
static_assert(!ReusableAsStencilRecord(false, false, false, true));

int main() {
    // A former color target at the stencil's guest address is not a
    // bookkeeping record, even if its dimensions match the depth owner.
    assert(!ReusableAsStencilRecord(true, false, false, false));
    // An unrelated depth target must not be adopted into the stencil plane.
    assert(!ReusableAsStencilRecord(false, false, false, true));
    // One standalone stencil bookkeeping image may be associated.
    assert(ReusableAsStencilRecord(false, false, false, false));
    return 0;
}
