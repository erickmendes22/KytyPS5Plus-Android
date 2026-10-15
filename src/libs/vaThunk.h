#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_VA_THUNK_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_VA_THUNK_H_

#include "libs/vaContext.h"
#include "loader/guestAbi.h"

#include <bit>
#include <cstddef>
#include <cstdint>

// Hand thunks for guest variadic functions (printf, snprintf, ...). The generic marshaler cannot
// express a C variadic signature, so these functions bind their CallFrame directly.
//
// A variadic guest call arrives as the same register image the x86-64 VA_ARGS path sees: RDI..R9 and
// XMM0..XMM7 plus the stack. BuildVaContext turns that image into the VaContext that VA_CONTEXT
// (vaContext.h) builds on x86, so the existing ctx consumers (GetGuestPrintfCtxFunc and friends)
// read the arguments exactly as they do natively.

namespace Libs {

using VaCtxConsumer = int (*)(VaContext* ctx);

// Fills ctx from the caller's registers and stack. Mirrors VA_CONTEXT:
//   gp[0..5] = RDI, RSI, RDX, RCX, R8, R9 (named and variadic integer arguments alike)
//   fp[0..7] = XMM0..XMM7 (the full 128-bit register)
//   overflow_arg_area = the guest address of stack slot 0. VA_ARGS consumes six integer registers and
//   then names slot 0 as its seventh parameter, so the overflow area starts there even when fewer
//   named arguments exist. Variadic thunks here take at most three named integer arguments
//   (snprintf), all in registers, so no named argument ever occupies a stack slot.
// The caller declares ctx as `alignas(16) VaContext ctx {};`.
inline void BuildVaContext(const Loader::GuestAbi::CallFrame& frame, VaContext& ctx) {
	for (std::size_t i = 0; i < 6; ++i) {
		ctx.reg_save_area.gp[i] = frame.IntArg(static_cast<int>(i));
	}
	for (std::size_t i = 0; i < 8; ++i) {
		const Loader::GuestAbi::Xmm xmm = frame.VecArg(static_cast<int>(i));
		ctx.reg_save_area.fp[i]         = std::bit_cast<VaXmm>(xmm);
	}
	ctx.va_list.reg_save_area     = &ctx.reg_save_area;
	ctx.va_list.gp_offset         = static_cast<uint32_t>(offsetof(VaRegSave, gp));
	ctx.va_list.fp_offset         = static_cast<uint32_t>(offsetof(VaRegSave, fp));
	ctx.va_list.overflow_arg_area =
	    reinterpret_cast<void*>(static_cast<uintptr_t>(frame.StackArgAddress(0)));
}

// Runs a ctx consumer on the variadic arguments of a trapped guest call.
inline int RunVariadic(const Loader::GuestAbi::CallFrame& frame, VaCtxConsumer consume) {
	alignas(16) VaContext ctx {};
	BuildVaContext(frame, ctx);
	return consume(&ctx);
}

// Guest int return: the C int result is sign-extended to 64 bits, as a compiled guest reads EAX.
inline void SetIntResult(Loader::GuestAbi::CallFrame& frame, int value) {
	frame.SetIntReturn(static_cast<uint64_t>(static_cast<int64_t>(value)));
}

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_VA_THUNK_H_ */
