#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_HOST_FUNCTION_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_HOST_FUNCTION_H_

// Native (host) functions as seen by guest code, and host threads as guest threads.
//
// Guest x86-64 code can only jump into native code through an x86 trap stub on ARM64 (see
// cpu/guestCpu.h). Every place that hands a native function's address to guest code goes through
// this header:
//   x86 hosts    the native address itself (no change in behaviour)
//   ARM64 hosts  a trap stub, cached per key, that runs the function's HleThunk. A function without a
//                thunk gets a trap that logs its label and exits, so the missing piece shows by name.
//
// Every host thread that calls guest code must be attached (AttachGuestThread), or must call
// EnsureGuestThread before the first guest call (see guestCall.h).

#include "loader/guestAbi.h"
#include "loader/guestAbiInvoke.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <string_view>

namespace Loader::GuestHost {

// Guest-visible address of the native function at native_address. key identifies the function for
// caching; label names it in diagnostics. thunk is the ARM64 entry (nullptr: trap that exits).
uint64_t Address(std::string_view key, std::string_view label, uint64_t native_address,
                 GuestAbi::HleThunk thunk);

// Guest-visible address of a native function Fn with a plain signature. On ARM64 the trap label is
// the function's name, so the key must be unique per function.
template <auto Fn>
uint64_t AddressOf(std::string_view key) {
#if KYTY_HOST_ARM64
	return Address(key, key, reinterpret_cast<uint64_t>(Fn), GuestAbi::HleThunkFor<Fn>());
#else
	(void)key;
	return reinterpret_cast<uint64_t>(Fn);
#endif
}

// Guest-visible address of a symbol. HLE functions (host_function, Func type) become traps on
// ARM64; data symbols and guest exports keep their address.
uint64_t SymbolAddress(const SymbolRecord& record);

// Guest-native replacement for an HLE function (ARM64): when a symbol resolves to the native function
// at `native_address`, the guest gets the x86 code `factory` installs instead of a trap, so hot
// calls that need no host state (a counter read, a TLS base) skip the trap round trip. The factory
// runs once, at the first resolution, after the translator is up.
using GuestNativeFactory = uint64_t (*)();
void RegisterGuestNative(uint64_t native_address, GuestNativeFactory factory);

// Label of the trap whose thunk is running on this thread ("" outside a trap).
std::string_view CurrentTrapName();

// Uncached trap for a native function that is not a symbol (ARM64 only). Each call makes a new stub.
uint64_t NewTrap(std::string_view label, GuestAbi::HleThunk thunk);

// Attaches the calling host thread to the guest CPU with the given guest stack top and sets its guest
// TLS base. Returns false if the translator cannot start. x86 hosts: returns true, does nothing.
bool AttachGuestThread(uint64_t stack_top);

// Supplies the guest TLS base (TCB) of the calling thread, read by AttachGuestThread. Registered once by
// the runtime linker; without it attached threads keep FS base 0.
using ThreadTcbProvider = uint64_t (*)();
void SetThreadTcbProvider(ThreadTcbProvider provider);

// Attaches the calling host thread on first use with its own guest stack, detached at thread exit.
// No-op when already attached and on x86 hosts.
void EnsureGuestThread();

} // namespace Loader::GuestHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUEST_HOST_FUNCTION_H_ */
