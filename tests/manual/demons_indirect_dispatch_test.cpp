#include "graphics/guest_gpu/command_processor/indirectDrawPacketPolicy.h"
#include "graphics/host_gpu/renderer/indirectDispatchPolicy.h"

using Libs::Graphics::ChooseIndirectDispatchSource;
using Libs::Graphics::IndirectDispatchSource;
using Libs::Graphics::ReserveIndirectLdsFromGpu;

// GPU-origin dispatch counts may already have been published into a cached
// Vulkan buffer, clearing dirty tracking while leaving the CPU's snapshot
// zero. Full preallocated LDS storage is required by every opted-in indirect
// dispatch, independent of CPU dirty flags and without extra allocation.
static_assert(ReserveIndirectLdsFromGpu(0x21c77f7dcull, true));
static_assert(ReserveIndirectLdsFromGpu(0x21c77f7f4ull, true));
static_assert(!ReserveIndirectLdsFromGpu(0x21c77f7dcull, false));
static_assert(!ReserveIndirectLdsFromGpu(0ull, true));

// No extra work is emitted when the GPU argument record really contains zero;
// only the unsafe early CPU return is suppressed for GPU-authored arguments.
static_assert(ChooseIndirectDispatchSource(true, true, false) == IndirectDispatchSource::Skip);
static_assert(ChooseIndirectDispatchSource(true, true, true) == IndirectDispatchSource::Gpu);
static_assert(ChooseIndirectDispatchSource(false, true, false) == IndirectDispatchSource::Gpu);
static_assert(ChooseIndirectDispatchSource(false, true, true) == IndirectDispatchSource::Gpu);
static_assert(ChooseIndirectDispatchSource(true, false, true) == IndirectDispatchSource::Cpu);
static_assert(ChooseIndirectDispatchSource(true, false, false) == IndirectDispatchSource::Cpu);

int main() { return 0; }

namespace P = Libs::Graphics::IndirectDrawPacketPolicy;
static_assert(P::SubdrawRegister(0x000002a5u) == P::NoRegister);
static_assert(P::SubdrawRegister(0x800002a5u) == 0x2a5u);
static_assert(P::SubdrawRegister(0xd00002a5u) == 0x2a5u);
static_assert(!P::RequiresCpu(P::NoRegister,P::NoRegister));
static_assert(P::RequiresCpu(0x2a0u,P::NoRegister));
static_assert(P::RequiresCpu(P::NoRegister,0x2a5u));
