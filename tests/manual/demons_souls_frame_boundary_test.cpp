#include "graphics/guest_gpu/guestFramePolicy.h"
#include <cassert>
#include <cstdio>
#include <set>
using Libs::Graphics::GuestFramePolicy;

// Synthetic interleaved graphics/compute/flip submissions: the GPU processor
// can complete them out of order, but the guest frame may advance only when
// no in-flight sequence is <= the prior frame's boundary.
int main() {
    GuestFramePolicy state;
    std::set<uint64_t> pending;
    const auto graphics0 = state.Admit(); pending.insert(graphics0);
    const auto compute0 = state.Admit(); pending.insert(compute0);
    const auto flip0 = state.Admit(); pending.insert(flip0);
    assert(graphics0 == 1 && compute0 == 2 && flip0 == 3);
    assert(!state.ConsumeGraphicsReset());
    assert(state.EndFrame() == 0); // first suspend does not stall
    assert(state.ConsumeGraphicsReset()); // reset belongs to next graphics job
    assert(!state.ConsumeGraphicsReset()); // never reset twice
    const auto graphics1 = state.Admit(); pending.insert(graphics1);
    assert(state.EndFrame() == 3); // wait for frame 0's gfx AND compute AND flip
    pending.erase(flip0); pending.erase(graphics0);
    assert(*pending.begin() == compute0); // compute still active, cannot pass
    pending.erase(compute0);
    assert(*pending.begin() == graphics1 && *pending.begin() > 3);
    assert(state.ConsumeGraphicsReset());
    assert(state.EndFrame() == 4);
    pending.erase(graphics1);
    assert(pending.empty());
    std::puts("Demon's Souls frame-order parity: no premature reset, complete per-queue prefix");
}
