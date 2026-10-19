// Guest address folding in the translator (see kernel/addressFold.h and the FEX patch
// src/cpu/patches/fex-addressfold.patch).

#include "cpu/guestCpuFex.h"

#include <FEXCore/Core/GuestAddressFold.h>

#include <cstdint>
#include <cstdio>

namespace Cpu::GuestCpu {

void EnableAddressFold(uint64_t mask) {
	if (FEXCore::GuestAddressFold::GetMask() == mask) {
		return;
	}
	FEXCore::GuestAddressFold::SetMask(mask);

	// Blocks compiled so far access memory unfolded. Drop them from every thread's lookup caches so
	// they are recompiled with the mask; a block that is running right now finishes as compiled.
	auto& runtime = Fex::GetRuntime();
	if (runtime.syscalls == nullptr) {
		return; // translator not started yet: nothing compiled to drop
	}
	const auto* frame = Fex::g_hle_frame;
	auto*       thread  = frame != nullptr ? frame->Thread : nullptr;
	constexpr uint64_t kAllGuestCode = uint64_t {1} << 47;
	runtime.syscalls->TM.InvalidateGuestCodeRange(thread, 0, kAllGuestCode);
	std::fprintf(stderr, "kyty guest cpu: address folding on, mask 0x%016llx\n",
	             static_cast<unsigned long long>(mask));
}

} // namespace Cpu::GuestCpu
