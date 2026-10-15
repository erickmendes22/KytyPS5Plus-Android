#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_INVOKE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_INVOKE_H_

#include "loader/guestAbi.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

// Marshaling from a guest x86-64 SysV call (described by a CallFrame) into a native C++ call of an
// HLE function, and back into the return registers.
//
// The argument plan is computed at compile time from Fn's signature, using the SysV classification:
//   INTEGER  integral (incl. bool/char), enum, pointer, reference (passed as its address), nullptr_t
//   SSE      float, double (low 32/64 bits of the XMM register)
// Each argument takes the next free register of its class. Once a class is exhausted the argument
// takes the next 8-byte stack slot; stack slots are shared by both classes and consumed left to
// right. Narrow integers are truncated from the 64-bit register value.
//
// Rejected with static_assert: by-value struct/union/class, long double, __int128 parameters,
// variadic functions and non-function callables. A function with such a signature opts out by
// specializing HleManualThunk<Fn> as std::true_type; HleThunkFor<Fn>() then yields nullptr and the
// function must be bound by hand.

namespace Loader::GuestAbi {

// Opt-out for functions that cannot be marshaled generically. Specialize per function, in the
// translation unit that registers it, before the registration.
template <auto Fn>
struct HleManualThunk : std::false_type {};

namespace Detail {

inline constexpr std::size_t kIntArgRegs = 6; // RDI, RSI, RDX, RCX, R8, R9
inline constexpr std::size_t kSseArgRegs = 8; // XMM0..XMM7

template <typename T>
inline constexpr bool kDependentFalse = false;

enum class ArgClass { Integer, Sse };

struct ArgLocation {
	ArgClass    cls;
	bool        on_stack;
	std::size_t index; // register index within its class, or 8-byte stack slot index
};

template <typename... P>
struct ParamList {};

#if defined(__SIZEOF_INT128__)
template <typename T>
inline constexpr bool kIsInt128 =
    std::is_same_v<T, __int128> || std::is_same_v<T, unsigned __int128>;
#else
template <typename T>
inline constexpr bool kIsInt128 = false;
#endif

// x86-64 char and wchar_t are signed. The host may disagree (char is unsigned on ARM64 Linux), so
// return values follow the guest's signedness, not the host's.
template <typename T>
inline constexpr bool kGuestSigned =
    std::is_signed_v<T> || std::is_same_v<T, char> || std::is_same_v<T, wchar_t>;

template <typename T>
constexpr ArgClass ClassifyArg() {
	using U = std::remove_cv_t<T>;
	if constexpr (kIsInt128<U>) {
		static_assert(kDependentFalse<U>,
		              "HLE thunk: __int128 parameter needs two INTEGER registers; not supported. "
		              "Opt out with HleManualThunk.");
		return ArgClass::Integer;
	} else if constexpr (std::is_same_v<U, float> || std::is_same_v<U, double>) {
		return ArgClass::Sse;
	} else if constexpr (std::is_integral_v<U> || std::is_enum_v<U> || std::is_pointer_v<U> ||
	                     std::is_reference_v<U> || std::is_null_pointer_v<U>) {
		return ArgClass::Integer;
	} else if constexpr (std::is_floating_point_v<U>) {
		static_assert(kDependentFalse<U>,
		              "HLE thunk: long double is passed in x87 memory, not supported. "
		              "Opt out with HleManualThunk.");
		return ArgClass::Sse;
	} else if constexpr (std::is_member_pointer_v<U>) {
		static_assert(kDependentFalse<U>,
		              "HLE thunk: member pointer parameter is not a C ABI type. "
		              "Opt out with HleManualThunk.");
		return ArgClass::Integer;
	} else if constexpr (std::is_class_v<U> || std::is_union_v<U>) {
		static_assert(kDependentFalse<U>,
		              "HLE thunk: struct/union/class passed by value needs SysV aggregate "
		              "classification, which is not implemented. Opt out with HleManualThunk.");
		return ArgClass::Integer;
	} else {
		static_assert(kDependentFalse<U>, "HLE thunk: unsupported parameter type.");
		return ArgClass::Integer;
	}
}

// Assigns every parameter its register or stack slot. Each argument is placed independently: an
// argument that does not fit in the remaining registers of its class goes to the stack, and later
// arguments of the other class may still use registers.
template <typename... P>
constexpr std::array<ArgLocation, sizeof...(P)> PlanArgs() {
	constexpr std::array<ArgClass, sizeof...(P)> kClasses {ClassifyArg<P>()...};
	std::array<ArgLocation, sizeof...(P)>        out {};
	std::size_t                                  int_used   = 0;
	std::size_t                                  sse_used   = 0;
	std::size_t                                  stack_used = 0;
	for (std::size_t i = 0; i < sizeof...(P); ++i) {
		const ArgClass cls = kClasses[i];
		if (cls == ArgClass::Integer && int_used < kIntArgRegs) {
			out[i] = {cls, false, int_used++};
		} else if (cls == ArgClass::Sse && sse_used < kSseArgRegs) {
			out[i] = {cls, false, sse_used++};
		} else {
			out[i] = {cls, true, stack_used++};
		}
	}
	return out;
}

template <typename P>
P DecodeInteger(uint64_t raw) {
	if constexpr (std::is_reference_v<P>) {
		auto* object = reinterpret_cast<std::remove_reference_t<P>*>(static_cast<uintptr_t>(raw));
		return static_cast<P>(*object);
	} else if constexpr (std::is_pointer_v<P>) {
		return reinterpret_cast<P>(static_cast<uintptr_t>(raw));
	} else if constexpr (std::is_null_pointer_v<P>) {
		return nullptr;
	} else if constexpr (std::is_enum_v<P>) {
		return static_cast<P>(static_cast<std::underlying_type_t<P>>(raw));
	} else if constexpr (std::is_same_v<P, bool>) {
		// Only the low byte of a _Bool argument is defined by the ABI; upper bits are garbage.
		return (raw & 0xFFu) != 0;
	} else {
		static_assert(std::is_integral_v<P>);
		return static_cast<P>(raw); // modular truncation to the native width
	}
}

template <typename P>
P DecodeSse(uint64_t raw) {
	if constexpr (std::is_same_v<P, float>) {
		return std::bit_cast<float>(static_cast<uint32_t>(raw));
	} else {
		static_assert(std::is_same_v<P, double>);
		return std::bit_cast<double>(raw);
	}
}

template <typename P>
P FetchArg(const CallFrame& f, const ArgLocation& loc) {
	const int index = static_cast<int>(loc.index);
	if constexpr (ClassifyArg<P>() == ArgClass::Sse) {
		const uint64_t raw = loc.on_stack ? f.StackArg(index) : f.VecArg(index).lo;
		return DecodeSse<P>(raw);
	} else {
		const uint64_t raw = loc.on_stack ? f.StackArg(index) : f.IntArg(index);
		return DecodeInteger<P>(raw);
	}
}

template <typename U>
void StoreInteger(CallFrame& f, U value) {
	if constexpr (kGuestSigned<U>) {
		// Sign-extend from the declared width, as a C compiler on the guest side does.
		using Signed = std::make_signed_t<U>;
		f.SetIntReturn(static_cast<uint64_t>(static_cast<int64_t>(static_cast<Signed>(value))));
	} else {
		f.SetIntReturn(static_cast<uint64_t>(value));
	}
}

template <typename Ret>
void StoreResult(CallFrame& f, Ret value) {
	if constexpr (std::is_reference_v<Ret>) {
		f.SetIntReturn(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(std::addressof(value))));
	} else if constexpr (std::is_same_v<Ret, float>) {
		f.SetVecReturn(Xmm {std::bit_cast<uint32_t>(value), 0});
	} else if constexpr (std::is_same_v<Ret, double>) {
		f.SetVecReturn(Xmm {std::bit_cast<uint64_t>(value), 0});
	} else if constexpr (std::is_null_pointer_v<Ret>) {
		f.SetIntReturn(0);
	} else if constexpr (std::is_pointer_v<Ret>) {
		f.SetIntReturn(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
	} else if constexpr (kIsInt128<Ret>) {
		f.SetIntReturn(static_cast<uint64_t>(value));
		f.SetIntReturn2(static_cast<uint64_t>(value >> 64));
	} else if constexpr (std::is_enum_v<Ret>) {
		StoreInteger(f, static_cast<std::underlying_type_t<Ret>>(value));
	} else if constexpr (std::is_integral_v<Ret>) {
		StoreInteger(f, value);
	} else {
		static_assert(kDependentFalse<Ret>,
		              "HLE thunk: unsupported return type (struct/union/long double). "
		              "Opt out with HleManualThunk.");
	}
}

template <typename Sig>
struct SigTraits {
	static_assert(kDependentFalse<Sig>,
	              "HLE thunk: Fn must be a plain (non-member) function or function pointer.");
};

template <typename R, typename... P>
struct SigTraits<R(P...)> {
	using Ret    = R;
	using Params = ParamList<P...>;
};

template <typename R, typename... P>
struct SigTraits<R(P...) noexcept> : SigTraits<R(P...)> {};

template <typename R, typename... P>
struct SigTraits<R(P..., ...)> {
	static_assert(kDependentFalse<R>,
	              "HLE thunk: variadic functions cannot be marshaled from registers (va_list is a "
	              "native struct on ARM64). Opt out with HleManualThunk.");
	using Ret    = R;
	using Params = ParamList<>;
};

template <typename R, typename... P>
struct SigTraits<R(P..., ...) noexcept> : SigTraits<R(P..., ...)> {};

template <auto Fn, typename Ret, typename... P>
void InvokeWith(CallFrame& f, ParamList<P...> /*params*/) {
	constexpr auto kLocations = PlanArgs<P...>();
	[&]<std::size_t... I>(std::index_sequence<I...>) {
		if constexpr (std::is_void_v<Ret>) {
			Fn(FetchArg<P>(f, kLocations[I])...);
		} else {
			StoreResult<Ret>(f, Fn(FetchArg<P>(f, kLocations[I])...));
		}
	}(std::index_sequence_for<P...> {});
}

} // namespace Detail

// Calls the native function Fn with the arguments the guest passed in CallFrame and stores its
// result into the guest return registers.
template <auto Fn>
void InvokeHle(CallFrame& f) {
	using Traits = Detail::SigTraits<std::remove_pointer_t<decltype(Fn)>>;
	Detail::InvokeWith<Fn, std::remove_cv_t<typename Traits::Ret>>(f, typename Traits::Params {});
}

// Thunk for registration: &InvokeHle<Fn>, or nullptr when Fn opted out via HleManualThunk.
template <auto Fn>
constexpr HleThunk HleThunkFor() {
	if constexpr (HleManualThunk<Fn>::value) {
		return nullptr;
	} else {
		return &InvokeHle<Fn>;
	}
}

} // namespace Loader::GuestAbi

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_INVOKE_H_ */
