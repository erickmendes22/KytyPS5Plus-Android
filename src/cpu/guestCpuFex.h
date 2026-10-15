#ifndef KYTY_CPU_GUEST_CPU_FEX_H_
#define KYTY_CPU_GUEST_CPU_FEX_H_

// Private to src/cpu/guestCpuFex*.cpp. Pulls in FEX headers, so nothing outside src/cpu may include
// it. Public API is src/cpu/guestCpu.h.

#include "cpu/fexSymbolPrefix.h" // must precede every FEX header

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Utils/LogManager.h>

#include "LinuxSyscalls/SignalDelegator.h"
#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/ThreadManager.h"
#include "Thunks.h"

#include "cpu/guestCpu.h"

#include <cstdint>

namespace Cpu::GuestCpu::Fex {

// Syscall number reserved for HLE traps. Guest stubs issue `syscall` with this number.
inline constexpr int kHleSyscallNumber = 400;

struct Runtime {
	FEXCore::Context::Context*      ctx      = nullptr;
	FEX::HLE::SignalDelegator*      signals  = nullptr;
	FEX::HLE::SyscallHandler*       syscalls = nullptr;
	FEX::HLE::ThunkHandler*         thunks   = nullptr;
	bool                            sve256   = false;
};

// Valid once Init() has succeeded.
Runtime& GetRuntime();

// Innermost guest frame while an HLE thunk runs on this thread; null otherwise.
inline thread_local FEXCore::Core::CpuStateFrame* g_hle_frame = nullptr;

// Stub id of the trap being serviced on this thread; kNoHleId outside a thunk.
inline thread_local uint32_t g_hle_id = kNoHleId;

// Syscall-400 entry point installed by Init(). Dispatches to the thunk registered for the stub id.
uint64_t HleSyscall(FEXCore::Core::CpuStateFrame* frame, uint64_t arg0, uint64_t arg1, uint64_t arg2,
                    uint64_t arg3, uint64_t arg4, uint64_t arg5);

// Returns the thunk for id, or null when none is registered.
HleThunk LookupHle(uint32_t id);

} // namespace Cpu::GuestCpu::Fex

#endif /* KYTY_CPU_GUEST_CPU_FEX_H_ */
