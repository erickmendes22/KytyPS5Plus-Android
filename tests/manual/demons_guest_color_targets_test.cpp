#include "graphics/host_gpu/renderer/colorOutputRecovery.h"

#include <cassert>
#include <cstdint>

using Libs::Graphics::RecoverGuestColorTargets;

static_assert(RecoverGuestColorTargets(0x00u, 0x01u, 0x01u) == 0x01u);
static_assert(RecoverGuestColorTargets(0x01u, 0x01u, 0x01u) == 0x01u);
static_assert(RecoverGuestColorTargets(0x01u, 0x03u, 0x03u) == 0x03u);
static_assert(RecoverGuestColorTargets(0x00u, 0x03u, 0x02u) == 0x02u);
static_assert(RecoverGuestColorTargets(0x00u, 0x00u, 0xffu) == 0x00u);
static_assert(RecoverGuestColorTargets(0x20u, 0x02u, 0x00u) == 0x20u);

int main() {
    // An unidentified guest MRT0 must be retained only if it has backing.
    assert(RecoverGuestColorTargets(0u, 1u, 1u) == 1u);
    assert(RecoverGuestColorTargets(0u, 1u, 0u) == 0u);
    // Nonzero MRT slots cannot overwrite an unrelated already-discovered MRT.
    assert(RecoverGuestColorTargets(1u, 4u, 4u) == 5u);
    // Never construct a target solely because the base address was nonzero.
    assert(RecoverGuestColorTargets(0u, 0u, 8u) == 0u);
    return 0;
}
