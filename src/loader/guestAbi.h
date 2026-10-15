#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_H_

#include <array>
#include <cstdint>

// Guest (x86-64 SysV) -> host call boundary for hosts where guest code runs under a binary
// translator (ARM64 + FEXCore). Guest code reaches a native HLE function through a small x86 stub
// that traps into the translator; the CPU backend then hands the HLE thunk a CallFrame describing
// the guest caller's argument registers and stack, and the thunk marshals them into a normal native
// C++ call (see guestAbiInvoke.h).
//
// Contract between the two sides:
// - The CPU backend (src/cpu, FEX) implements CallFrame for a trapped guest call.
// - The marshaling layer implements one HleThunk per LIB_FUNC and only talks to CallFrame.

namespace Loader::GuestAbi {

struct Xmm {
	uint64_t lo;
	uint64_t hi;
};

// SysV class of one eightbyte of a by-value argument or return value.
enum class ArgClass { Integer, Sse };

// Declares the SysV eightbyte classes of a by-value struct/union/class of at most 16 bytes. Specialize
// it next to the type, with one entry per eightbyte (ceil(sizeof(T) / 8) entries):
//   template <> struct HleAggregate<Pair> {
//       static constexpr std::array<ArgClass, 1> kEightbytes {ArgClass::Integer};
//   };
// An eightbyte is Sse only when every scalar overlapping it is float/double; otherwise Integer
// (SysV 3.2.3). Larger aggregates are MEMORY class and need no declaration. Only specialize for
// trivially copyable types whose fields are all naturally aligned (no packed members).
template <typename T>
struct HleAggregate {};

// View of the guest CPU at an HLE call boundary, as seen by the guest caller (i.e. after the trap
// stub has been accounted for: IntArg(3) is the caller's RCX even if the stub moved it).
class CallFrame {
public:
	CallFrame()          = default;
	virtual ~CallFrame() = default;

	CallFrame(const CallFrame&)            = delete;
	CallFrame& operator=(const CallFrame&) = delete;

	// i-th INTEGER-class argument register: 0..5 -> RDI, RSI, RDX, RCX, R8, R9.
	[[nodiscard]] virtual uint64_t IntArg(int index) const = 0;

	// i-th SSE-class argument register: 0..7 -> XMM0..XMM7 (low 128 bits).
	[[nodiscard]] virtual Xmm VecArg(int index) const = 0;

	// i-th 8-byte stack argument slot in the caller's frame: slot 0 is the first argument passed in
	// memory ([RSP + 8] at the moment the guest executed CALL).
	[[nodiscard]] virtual uint64_t StackArg(int index) const = 0;

	// Guest address of the i-th stack argument slot, i.e. the address StackArg(index) reads from.
	// Guest memory is host-addressable, so the value can be used as a pointer. Variadic thunks use
	// StackArgAddress(0) as the va_list overflow_arg_area (the first memory slot after the named
	// arguments, which all fit in registers for printf/snprintf).
	[[nodiscard]] virtual uint64_t StackArgAddress(int index) const = 0;

	virtual void SetIntReturn(uint64_t rax)  = 0; // RAX
	virtual void SetIntReturn2(uint64_t rdx) = 0; // RDX (second eightbyte of a 128-bit return)
	virtual void SetVecReturn(Xmm xmm0)      = 0; // XMM0
};

// One per exported HLE function; marshals CallFrame -> native call -> return registers.
using HleThunk = void (*)(CallFrame& frame);

} // namespace Loader::GuestAbi

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_H_ */
