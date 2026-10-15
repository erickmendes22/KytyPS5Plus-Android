#ifndef KYTY_CPU_GUEST_CPU_H_
#define KYTY_CPU_GUEST_CPU_H_

// Arch-neutral interface to the guest x86-64 CPU.
//
// On ARM64 hosts guest code runs inside FEXCore (binary translation) and this interface is the only
// way native code enters or leaves it. Native HLE functions are reached from guest code through
// x86 trap stubs (MakeHleStub) that the translator forwards to the registered HleThunk.
//
// This header must stay free of FEX and fmt includes: it is consumed by code that also uses Kyty's
// own fmt copy, while the FEX implementation is built against a separately namespaced fmt.

#include "loader/guestAbi.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Cpu::GuestCpu {

using HleThunk = Loader::GuestAbi::HleThunk;

// Host-first SIGSEGV hook. Runs before the translator's own handling; return true to claim the
// fault (the faulting guest instruction is then retried), false to pass it on.
using HostFaultHandler = bool (*)(uintptr_t fault_address);

// Largest number of integer arguments CallGuest passes in registers (RDI, RSI, RDX, RCX, R8, R9).
inline constexpr size_t kMaxCallArgs = 6;

// Brings up the translator once per process. Safe to call repeatedly. Returns false on failure.
bool Init();

// Binds the calling host thread to a guest thread context whose default guest stack ends at
// guest_stack_top. Must be paired with DetachCurrentThread on the same host thread.
bool AttachCurrentThread(uint64_t guest_stack_top);
void DetachCurrentThread();
bool IsCurrentThreadAttached();

// Sets the guest FS base of the innermost guest context on the calling thread.
void SetGuestFsBase(uint64_t base);

// Runs guest code at entry to its return on the calling thread and returns RAX. Integer arguments go
// to RDI..R9. Uses the attached thread's stack, or, when called from inside an HLE thunk, the guest
// stack below the interrupted guest RSP.
uint64_t CallGuest(uint64_t entry, std::span<const uint64_t> int_args);

// As CallGuest, but on an explicit guest stack (e.g. a main-entry or pthread stack).
uint64_t CallGuestOnStack(uint64_t stack_top, uint64_t entry, std::span<const uint64_t> int_args);

// Copies code into fresh executable guest-visible memory and returns its address.
uint64_t AllocateGuestCode(std::span<const uint8_t> code);

// Returns the guest address of a stub that, when called with SysV arguments, traps into the
// translator and runs the HleThunk registered for id (see RegisterHle). Ids must be < 2^31.
uint64_t MakeHleStub(uint32_t id);

// Registers the thunk for id. Re-registering an id replaces the previous thunk.
void RegisterHle(uint32_t id, HleThunk thunk);

// Ids below kFirstDynamicHleId are for fixed, hand-chosen registrations. AllocateHleId hands out ids
// from kFirstDynamicHleId upward, for traps created at run time (one per host function a guest sees).
inline constexpr uint32_t kFirstDynamicHleId = 64;
uint32_t AllocateHleId();

// Id of the trap whose thunk is running on the calling thread, or kNoHleId outside a thunk.
inline constexpr uint32_t kNoHleId = UINT32_MAX;
uint32_t CurrentHleId();


// Installs the process-wide host SIGSEGV hook. Only one handler is kept; later calls replace it.
void RegisterHostFaultHandler(HostFaultHandler handler);

// Tells the translator that [base, base + size) may hold guest code. FEX only decodes addresses
// its VMA table knows about, and Kyty maps guest memory with host mmap (bypassing FEX's syscall
// layer), so the guest address window is registered once. This is bookkeeping only: it maps
// nothing and does not change host page protections.
void RegisterGuestCodeRange(uint64_t base, uint64_t size);

} // namespace Cpu::GuestCpu

#endif /* KYTY_CPU_GUEST_CPU_H_ */
