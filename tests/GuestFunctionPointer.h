#ifndef KYTY_TESTS_GUEST_FUNCTION_POINTER_H_
#define KYTY_TESTS_GUEST_FUNCTION_POINTER_H_

// A native test function that the code under test calls as guest code. On ARM64 the call goes through
// CallGuest, so the pointer must be a guest-visible trap (loader/guestHostFunction.h); on x86 it is the
// function itself.

#include "common/abi.h"
#include "loader/guestHostFunction.h"

template <auto Fn>
auto GuestFunctionPointer(const char* key) {
#if KYTY_HOST_ARM64
	return reinterpret_cast<decltype(Fn)>(Loader::GuestHost::AddressOf<Fn>(key));
#else
	(void)key;
	return Fn;
#endif
}

#endif /* KYTY_TESTS_GUEST_FUNCTION_POINTER_H_ */
