#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_CALL_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_CALL_H_

// Host -> guest calls. A guest-provided function pointer (callback, destructor, init routine, ...) is
// called here instead of directly:
//   x86 hosts    a plain call through the pointer (the code is identical to calling it directly)
//   ARM64 hosts  Cpu::GuestCpu::CallGuest, which runs the x86 code under FEXCore
//
// Arguments: integer, pointer, enum and bool values (at most 6, all in registers). Floating point
// arguments are not supported: extend CallGuest before adding such a site. The result is converted
// to the declared return type (pointers, bool from AL, narrow integers truncated by the caller's type).

#include "common/abi.h"
#include "loader/guestHostFunction.h"

#if KYTY_HOST_ARM64
#include "cpu/guestCpu.h"
#endif

#include <array>
#include <cstdint>
#include <type_traits>

namespace Loader::GuestHost {

#if KYTY_HOST_ARM64
namespace Detail {

template <typename T>
uint64_t ToGuestWord(T value) {
	if constexpr (std::is_null_pointer_v<T>) {
		return 0;
	} else if constexpr (std::is_pointer_v<T>) {
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
	} else if constexpr (std::is_same_v<T, bool>) {
		return value ? 1u : 0u;
	} else if constexpr (std::is_enum_v<T>) {
		return static_cast<uint64_t>(static_cast<std::underlying_type_t<T>>(value));
	} else {
		static_assert(std::is_integral_v<T>, "CallGuestFunction: only integer and pointer arguments");
		return static_cast<uint64_t>(value);
	}
}

template <typename R>
R FromGuestWord(uint64_t raw) {
	if constexpr (std::is_pointer_v<R>) {
		return reinterpret_cast<R>(static_cast<uintptr_t>(raw));
	} else if constexpr (std::is_same_v<R, bool>) {
		return (raw & 0xffu) != 0;
	} else if constexpr (std::is_enum_v<R>) {
		return static_cast<R>(static_cast<std::underlying_type_t<R>>(raw));
	} else {
		static_assert(std::is_integral_v<R>, "CallGuestFunction: only integer and pointer results");
		return static_cast<R>(raw);
	}
}

} // namespace Detail
#endif

// Calls the guest function pointer fn with args and returns its result.
template <typename Fn, typename... Args>
decltype(auto) CallGuestFunction(Fn fn, Args... args) {
	static_assert(std::is_pointer_v<Fn>, "CallGuestFunction: fn must be a function pointer");
	using R = std::invoke_result_t<Fn, Args...>;
#if KYTY_HOST_ARM64
	EnsureGuestThread();
	const uint64_t raw = Cpu::GuestCpu::CallGuest(
	    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn)),
	    std::array<uint64_t, sizeof...(Args)> {Detail::ToGuestWord(args)...});
	if constexpr (std::is_void_v<R>) {
		(void)raw;
		return;
	} else {
		return Detail::FromGuestWord<R>(raw);
	}
#else
	return fn(args...);
#endif
}

} // namespace Loader::GuestHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_CALL_H_ */
