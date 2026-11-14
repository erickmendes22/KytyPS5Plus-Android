#include "graphics/host_gpu/renderer/colorWritePolicy.h"

#include <cassert>
using Libs::Graphics::GuestColorWriteEnabled;

// Target exists but shader exports no channels to it: no host writes.
static_assert(!GuestColorWriteEnabled(0x0000000fu, 0x00000000u, 0, true));
static_assert(GuestColorWriteEnabled(0x0000000fu, 0x0000000fu, 0, true));
static_assert(GuestColorWriteEnabled(0x00000002u, 0x00000006u, 0, true));
static_assert(!GuestColorWriteEnabled(0x00000001u, 0x00000002u, 0, true));
// Slot 1 must never use slot 0's mask.
static_assert(GuestColorWriteEnabled(0x000000f0u, 0x00000010u, 1, true));
static_assert(!GuestColorWriteEnabled(0x000000f0u, 0x0000000fu, 1, true));
// Missing host backing and invalid slots never write.
static_assert(!GuestColorWriteEnabled(0xffffffffu, 0xffffffffu, 0, false));
static_assert(!GuestColorWriteEnabled(0xffffffffu, 0xffffffffu, 8, true));

int main() {
    assert(GuestColorWriteEnabled(0x000000f0u, 0x00000030u, 1, true));
    assert(!GuestColorWriteEnabled(0x000000f0u, 0x00000000u, 1, true));
    return 0;
}
