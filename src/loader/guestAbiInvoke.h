#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_INVOKE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_ABI_INVOKE_H_

#include "loader/guestAbi.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

// Marshaling from a guest x86-64 SysV call (described by a CallFrame) into a native C++ call of an
// HLE function, and back into the return registers.
//
// Classification (SysV 3.2.3), computed at compile time from Fn's signature:
//   Scalars (one eightbyte):
//     INTEGER  integral (incl. bool/char), enum, pointer, reference (passed as its address), nullptr_t
//     SSE      float, double (low 32/64 bits of the XMM register)
//   Aggregates (struct/union/class by value) of at most 16 bytes: one class per eightbyte, declared
//     by HleAggregate<T> (guestAbi.h). Eightbytes take consecutive registers of their class, in order
//     (e.g. {float x, y; float z} takes XMM n and XMM n+1).
//   Aggregates larger than 16 bytes are MEMORY class: copied into ceil(size / 8) stack slots.
// An argument takes the next free registers of its classes; if they do not all fit, the whole
// argument takes consecutive 8-byte stack slots instead. Stack slots are shared by both classes and
// consumed left to right; later arguments may still use registers after an argument spilled.
//
// Returns: a scalar uses RAX / XMM0. An aggregate of at most 16 bytes returns its INTEGER eightbytes
// in RAX then RDX and its SSE eightbyte in XMM0 (at most two INTEGER and one SSE eightbyte; other
// mixes are rejected with static_assert). An aggregate larger than 16 bytes is returned through a
// hidden pointer: the caller passes it in RDI, so declared parameters start at RSI; the callee writes
// the value there and returns the pointer in RAX.
//
// Narrow integers are truncated from the 64-bit register value.
//
// Rejected with static_assert: long double, __int128 parameters (except as a return), member
// pointers, variadic functions, aggregates without an HleAggregate declaration. A function with such
// a signature opts out by specializing HleManualThunk<Fn> as std::true_type; HleThunkFor<Fn>() then
// yields nullptr and the function is bound by hand (variadic functions, see libs/vaThunk.h).

namespace Loader::GuestAbi {

// Opt-out for functions that cannot be marshaled generically. Specialize per function, in the
// translation unit that registers it, before the registration.
template <auto Fn>
struct HleManualThunk : std::false_type {};

// KYTY_HLE_TRACE (diagnostics): see InvokeWith.
namespace HleTrace {
// Read lazily: --env applies after static initialisation.
bool Enabled();
// True for the first calls of a function (its own counter), up to the limit.
bool Take(std::atomic<uint32_t>& counter);
void Log(const char* function, uint64_t a0, uint64_t a1, uint64_t a2, bool has_result, uint64_t result);
} // namespace HleTrace

template <auto Fn>
std::atomic<uint32_t>& HleTraceCounter() {
	static std::atomic<uint32_t> counter {0};
	return counter;
}

namespace Detail {

inline constexpr std::size_t kIntArgRegs           = 6; // RDI, RSI, RDX, RCX, R8, R9
inline constexpr std::size_t kSseArgRegs           = 8; // XMM0..XMM7
inline constexpr std::size_t kMaxRegAggregateBytes = 16;

template <typename T>
inline constexpr bool kDependentFalse = false;

template <typename T>
inline constexpr bool kIsAggregate = std::is_class_v<T> || std::is_union_v<T>;

// MEMORY class: an aggregate larger than 16 bytes. A function, not a variable template, so that
// sizeof is never evaluated for non-aggregates such as void.
template <typename T>
constexpr bool IsMemoryAggregate() {
	if constexpr (kIsAggregate<T>) {
		return sizeof(T) > kMaxRegAggregateBytes;
	} else {
		return false;
	}
}

template <typename T>
inline constexpr std::size_t kWordCount = (sizeof(T) + 7) / 8;

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
concept HasAggregateClasses = requires { HleAggregate<T>::kEightbytes; };

// Class of a scalar parameter or return type.
template <typename T>
constexpr ArgClass ClassifyScalar() {
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
	} else {
		static_assert(kDependentFalse<U>, "HLE thunk: unsupported parameter type.");
		return ArgClass::Integer;
	}
}

// How one parameter or return type travels: the registers it needs per class, and whether it is
// MEMORY class. slots is the number of 8-byte stack slots it occupies when passed on the stack.
struct Shape {
	std::size_t int_pieces = 0;
	std::size_t sse_pieces = 0;
	std::size_t slots      = 1;
	bool        memory     = false;
};

template <typename T>
constexpr Shape ShapeOf() {
	using U = std::remove_cv_t<T>;
	Shape shape {};
	if constexpr (kIsAggregate<U>) {
		static_assert(std::is_trivially_copyable_v<U>,
		              "HLE thunk: by-value aggregate must be trivially copyable.");
		static_assert(alignof(U) <= 8, "HLE thunk: by-value aggregate alignment above 8 bytes.");
		shape.slots = kWordCount<U>;
		if constexpr (IsMemoryAggregate<U>()) {
			shape.memory = true;
		} else {
			static_assert(HasAggregateClasses<U>,
			              "HLE thunk: by-value struct of at most 16 bytes needs an HleAggregate<T> "
			              "declaration (eightbyte classes). Declare it or opt out.");
			static_assert(HleAggregate<U>::kEightbytes.size() == kWordCount<U>,
			              "HLE thunk: HleAggregate<T>::kEightbytes needs one entry per eightbyte.");
			for (ArgClass cls: HleAggregate<U>::kEightbytes) {
				if (cls == ArgClass::Sse) {
					++shape.sse_pieces;
				} else {
					++shape.int_pieces;
				}
			}
		}
	} else if constexpr (ClassifyScalar<U>() == ArgClass::Sse) {
		shape.sse_pieces = 1;
	} else {
		shape.int_pieces = 1;
	}
	return shape;
}

// Where one argument is passed. A register argument takes consecutive registers of each class from
// the bases; a stack argument starts at stack_base.
struct ArgPlan {
	bool        on_stack;
	std::size_t int_base;
	std::size_t sse_base;
	std::size_t stack_base;
};

// Assigns every parameter its registers or stack slots. first_int is the first free INTEGER register
// (1 when a hidden return pointer occupies RDI).
template <typename... P>
constexpr std::array<ArgPlan, sizeof...(P)> PlanArgs(std::size_t first_int) {
	constexpr std::array<Shape, sizeof...(P)> kShapes {ShapeOf<P>()...};
	std::array<ArgPlan, sizeof...(P)>         out {};
	std::size_t                               int_used   = first_int;
	std::size_t                               sse_used   = 0;
	std::size_t                               stack_used = 0;
	for (std::size_t i = 0; i < sizeof...(P); ++i) {
		const Shape& s = kShapes[i];
		if (!s.memory && int_used + s.int_pieces <= kIntArgRegs &&
		    sse_used + s.sse_pieces <= kSseArgRegs) {
			out[i] = {false, int_used, sse_used, 0};
			int_used += s.int_pieces;
			sse_used += s.sse_pieces;
		} else {
			out[i] = {true, 0, 0, stack_used};
			stack_used += s.slots;
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

// Reassembles a by-value aggregate from its eightbytes: register eightbytes in order of their class,
// or consecutive stack slots when the argument (or the whole MEMORY-class value) is on the stack.
template <typename P>
P FetchAggregate(const CallFrame& f, const ArgPlan& plan) {
	constexpr std::size_t kWords = kWordCount<P>;
	std::array<uint64_t, kWords> words {};
	if constexpr (IsMemoryAggregate<P>()) {
		for (std::size_t k = 0; k < kWords; ++k) {
			words[k] = f.StackArg(static_cast<int>(plan.stack_base + k));
		}
	} else {
		constexpr auto kClasses = HleAggregate<P>::kEightbytes;
		std::size_t    next_int = plan.int_base;
		std::size_t    next_sse = plan.sse_base;
		for (std::size_t k = 0; k < kWords; ++k) {
			if (plan.on_stack) {
				words[k] = f.StackArg(static_cast<int>(plan.stack_base + k));
			} else if (kClasses[k] == ArgClass::Sse) {
				words[k] = f.VecArg(static_cast<int>(next_sse++)).lo;
			} else {
				words[k] = f.IntArg(static_cast<int>(next_int++));
			}
		}
	}
	P value {};
	std::memcpy(&value, words.data(), sizeof(P));
	return value;
}

template <typename P>
P FetchArg(const CallFrame& f, const ArgPlan& plan) {
	if constexpr (kIsAggregate<P>) {
		return FetchAggregate<std::remove_cv_t<P>>(f, plan);
	} else if constexpr (ClassifyScalar<P>() == ArgClass::Sse) {
		const int      index = static_cast<int>(plan.on_stack ? plan.stack_base : plan.sse_base);
		const uint64_t raw   = plan.on_stack ? f.StackArg(index) : f.VecArg(index).lo;
		return DecodeSse<P>(raw);
	} else {
		const int      index = static_cast<int>(plan.on_stack ? plan.stack_base : plan.int_base);
		const uint64_t raw   = plan.on_stack ? f.StackArg(index) : f.IntArg(index);
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

// Stores a by-value aggregate: MEMORY class through the hidden pointer in RDI, otherwise in RAX/RDX
// (INTEGER eightbytes) and XMM0 (SSE eightbyte).
template <typename R>
void StoreAggregate(CallFrame& f, const R& value) {
	if constexpr (IsMemoryAggregate<R>()) {
		const uint64_t hidden = f.IntArg(0);
		std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(hidden)), &value, sizeof(R));
		f.SetIntReturn(hidden);
	} else {
		constexpr auto kClasses = HleAggregate<R>::kEightbytes;
		constexpr auto kShape   = ShapeOf<R>();
		static_assert(kShape.int_pieces <= 2 && kShape.sse_pieces <= 1,
		              "HLE thunk: aggregate return needs RAX/RDX and at most XMM0; not supported. "
		              "Opt out with HleManualThunk.");
		std::array<uint64_t, kWordCount<R>> words {};
		std::memcpy(words.data(), &value, sizeof(R));
		std::size_t next_int = 0;
		for (std::size_t k = 0; k < kWordCount<R>; ++k) {
			if (kClasses[k] == ArgClass::Sse) {
				f.SetVecReturn(Xmm {words[k], 0});
			} else if (next_int++ == 0) {
				f.SetIntReturn(words[k]);
			} else {
				f.SetIntReturn2(words[k]);
			}
		}
	}
}

template <typename Ret>
void StoreResult(CallFrame& f, Ret value) {
	if constexpr (kIsAggregate<Ret>) {
		StoreAggregate(f, value);
	} else if constexpr (std::is_reference_v<Ret>) {
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
		              "HLE thunk: unsupported return type (long double). Opt out with HleManualThunk.");
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
	              "HLE thunk: variadic functions cannot be marshaled generically (the VA register "
	              "image is not a C signature). Opt out with HleManualThunk and bind by hand.");
	using Ret    = R;
	using Params = ParamList<>;
};

template <typename R, typename... P>
struct SigTraits<R(P..., ...) noexcept> : SigTraits<R(P..., ...)> {};

template <auto Fn, typename Ret, typename... P>
void InvokeWith(CallFrame& f, ParamList<P...> /*params*/) {
	// A MEMORY-class return consumes RDI for the hidden pointer.
	constexpr bool kHiddenReturn = IsMemoryAggregate<Ret>();
	constexpr auto kLocations    = PlanArgs<P...>(kHiddenReturn ? 1 : 0);
	// KYTY_HLE_TRACE=1: the first KYTY_HLE_TRACE_LIMIT (4) calls of each HLE function are logged
	// with their first integer arguments and integer result (HleTrace, runtimeLinker.cpp).
	const char* const name  = __PRETTY_FUNCTION__; // names Fn (the lambda's own does not)
	const bool        trace = HleTrace::Enabled() && HleTrace::Take(HleTraceCounter<Fn>());
	const uint64_t a0 = trace ? f.IntArg(0) : 0, a1 = trace ? f.IntArg(1) : 0, a2 = trace ? f.IntArg(2) : 0;
	[&]<std::size_t... I>(std::index_sequence<I...>) {
		if constexpr (std::is_void_v<Ret>) {
			Fn(FetchArg<P>(f, kLocations[I])...);
			if (trace) {
				HleTrace::Log(name, a0, a1, a2, false, 0);
			}
		} else {
			auto result = Fn(FetchArg<P>(f, kLocations[I])...);
			if (trace) {
				uint64_t value = 0;
				if constexpr (std::is_integral_v<Ret> || std::is_enum_v<Ret>) {
					value = static_cast<uint64_t>(result);
				} else if constexpr (std::is_pointer_v<Ret>) {
					value = reinterpret_cast<uint64_t>(result);
				}
				HleTrace::Log(name, a0, a1, a2, true, value);
			}
			StoreResult<Ret>(f, result);
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
