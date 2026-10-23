#include "common/hostException.h"
#include "common/hostFaultAccess.h"

#include "common/hostArch.h"
#include "common/processExitHooks.h"

#if KYTY_HOST_ARM64 && defined(KYTY_ANDROID)
#include "cpu/guestCpu.h"
#endif

#include <atomic>
#include <cstdio>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#else
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h> // IWYU pragma: keep
#if KYTY_HOST_ARM64
#include <asm/sigcontext.h> // esr_context, ESR_MAGIC
#endif
#endif
#endif

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::HostException {

#if !defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

// macOS uses the same POSIX platform setting and needs a signal stack too.
class ThreadSignalStack {
public:
	ThreadSignalStack() {
		const auto page_size = static_cast<size_t>(::getpagesize());
		const auto stack_size =
		    (std::max<size_t>(64 * 1024, MINSIGSTKSZ) + page_size - 1) & ~(page_size - 1);
		if (::posix_memalign(&m_memory, page_size, stack_size) != 0) {
			return;
		}

		stack_t stack {};
		stack.ss_sp   = m_memory;
		stack.ss_size = stack_size;
		if (::sigaltstack(&stack, &m_previous) != 0) {
			std::free(m_memory);
			m_memory = nullptr;
		}
	}

	~ThreadSignalStack() {
		if (m_memory != nullptr && ::sigaltstack(&m_previous, nullptr) == 0) {
			std::free(m_memory);
		}
	}

	[[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }

	KYTY_CLASS_NO_COPY(ThreadSignalStack)

private:
	void*   m_memory = nullptr;
	stack_t m_previous {};
};

bool InitializeThreadSignalStack() {
	// Keep fault handling off guest stacks, which GPU tracking can make read-only.
	thread_local ThreadSignalStack signal_stack;
	return signal_stack.IsInitialized();
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception) noexcept {
	auto* exception_record = exception->ExceptionRecord;

	if (exception_record->ExceptionCode == DBG_PRINTEXCEPTION_C ||
	    exception_record->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (exception_record->ExceptionCode == 0x406D1388) {
		// Set a thread name.
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	ExceptionInfo info {};
	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);
	info.native_code       = exception_record->ExceptionCode;
	info.native_context    = exception->ContextRecord;

	if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
		info.type = ExceptionType::AccessViolation;
		switch (exception_record->ExceptionInformation[0]) {
			case 0: info.access_violation_type = AccessViolationType::Read; break;
			case 1: info.access_violation_type = AccessViolationType::Write; break;
			case 8: info.access_violation_type = AccessViolationType::Execute; break;
			default: info.access_violation_type = AccessViolationType::Unknown; break;
		}
		info.access_violation_vaddr = exception_record->ExceptionInformation[1];
	} else if (exception_record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	info.rax = exception->ContextRecord->Rax;
	info.rbx = exception->ContextRecord->Rbx;
	info.rcx = exception->ContextRecord->Rcx;
	info.rdx = exception->ContextRecord->Rdx;
	info.rsi = exception->ContextRecord->Rsi;
	info.rdi = exception->ContextRecord->Rdi;
	info.rbp = exception->ContextRecord->Rbp;
	info.rsp = exception->ContextRecord->Rsp;
	info.r8  = exception->ContextRecord->R8;
	info.r9  = exception->ContextRecord->R9;
	info.r10 = exception->ContextRecord->R10;
	info.r11 = exception->ContextRecord->R11;
	info.r12 = exception->ContextRecord->R12;
	info.r13 = exception->ContextRecord->R13;
	info.r14 = exception->ContextRecord->R14;
	info.r15 = exception->ContextRecord->R15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);

// Translate the x86-64 page-fault error code (mcontext __es.__err) into an access type.
// bit 1 (0x2) = write, bit 4 (0x10) = instruction fetch, otherwise a read.
static AccessViolationType DecodeAccess(uint64_t err) {
	if ((err & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((err & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

// POSIX signal handler that mirrors the Windows vectored handler: build an ExceptionInfo
// from the mcontext and dispatch. A resolved fault (handler returns true) simply returns,
// re-executing the faulting instruction against the now-fixed protection. An unresolved
// fault restores the default disposition so the retry terminates the process.
static void SignalHandler(int sig, siginfo_t* si, void* uctx) {
	auto*       uc = static_cast<ucontext_t*>(uctx);
	const auto* mc = uc->uc_mcontext;
	const auto& ss = mc->__ss;

	ExceptionInfo info {};
	info.exception_address = ss.__rip;
	info.native_code       = static_cast<uint32_t>(si->si_code);
	info.native_context    = uctx;

	if (sig == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type                   = ExceptionType::AccessViolation;
		info.access_violation_type  = DecodeAccess(mc->__es.__err);
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(si->si_addr);
	}

	info.rax = ss.__rax;
	info.rbx = ss.__rbx;
	info.rcx = ss.__rcx;
	info.rdx = ss.__rdx;
	info.rsi = ss.__rsi;
	info.rdi = ss.__rdi;
	info.rbp = ss.__rbp;
	info.rsp = ss.__rsp;
	info.r8  = ss.__r8;
	info.r9  = ss.__r9;
	info.r10 = ss.__r10;
	info.r11 = ss.__r11;
	info.r12 = ss.__r12;
	info.r13 = ss.__r13;
	info.r14 = ss.__r14;
	info.r15 = ss.__r15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return; // retry the faulting instruction against the fixed mapping
	}

	// Unresolved: restore the default action so the re-executed instruction terminates.
	struct sigaction dfl {};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(sig, &dfl, nullptr);
}

#else

// x86-64 page-fault error bits.
constexpr uint64_t PAGE_FAULT_ERROR_WRITE       = 0x02;
constexpr uint64_t PAGE_FAULT_ERROR_INSTRUCTION = 0x10;

// Let the kernel handle an unresolved fault on retry.
static void ChainToDefault(int signal_number) noexcept {
	// The process dies when the fault is retried: undo external state (GPU clocks) first.
	RunProcessExitHooks();
	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	restore.sa_flags = 0;
	::sigaction(signal_number, &restore, nullptr);
}

#if defined(KYTY_ANDROID) && KYTY_HOST_ARM64
// The FEX signal delegator is installed before Kyty loads PS5 executables. Preserve its
// dispositions when Kyty adds its page-tracking handler. An unclaimed SIGSEGV must reach
// FEX so the translator can decode/recover a guest fault instead of being killed by the
// kernel on its next instruction. This is also necessary for its host-fault diagnostics.
static struct sigaction g_previous_sigsegv {};
static struct sigaction g_previous_sigbus {};
static struct sigaction g_previous_sigill {};

static struct sigaction* PreviousSignalAction(int signal_number) {
    switch (signal_number) {
        case SIGSEGV: return &g_previous_sigsegv;
        case SIGBUS:  return &g_previous_sigbus;
        case SIGILL:  return &g_previous_sigill;
        default:      return nullptr;
    }
}

static void ChainToPrevious(int signal_number, siginfo_t* info, void* context) {
    const auto* previous = PreviousSignalAction(signal_number);
    if (previous == nullptr || previous->sa_handler == SIG_DFL) {
        ChainToDefault(signal_number);
        return;
    }
    if (previous->sa_handler == SIG_IGN) return;
    // Forward the unchanged siginfo and ucontext to the FEX signal delegator. It may
    // recover a guest memory access, in which case execution simply resumes.
    if ((previous->sa_flags & SA_SIGINFO) != 0) {
        previous->sa_sigaction(signal_number, info, context);
    } else {
        previous->sa_handler(signal_number);
    }
}
#endif

#if KYTY_HOST_ARM64
// ESR_EL1 fields (ARM ARM D17.2.40): exception class and the data-abort write-not-read bit.
constexpr uint64_t ESR_EC_SHIFT               = 26;
constexpr uint64_t ESR_EC_MASK                = 0x3f;
constexpr uint64_t ESR_EC_INSTRUCTION_ABORT_L = 0x20;
constexpr uint64_t ESR_EC_INSTRUCTION_ABORT   = 0x21;
constexpr uint64_t ESR_DATA_ABORT_WNR         = 1ull << 6;

// The kernel appends an esr_context record to the signal frame for synchronous faults.
static uint64_t FindFaultEsr(const ucontext_t* context) {
	const auto* cursor = reinterpret_cast<const uint8_t*>(context->uc_mcontext.__reserved);
	const auto* end    = cursor + sizeof(context->uc_mcontext.__reserved);
	while (cursor + sizeof(_aarch64_ctx) <= end) {
		const auto* header = reinterpret_cast<const _aarch64_ctx*>(cursor);
		if (header->magic == 0 || header->size == 0) {
			break;
		}
		if (header->magic == ESR_MAGIC) {
			return reinterpret_cast<const esr_context*>(header)->esr;
		}
		cursor += header->size;
	}
	return 0;
}

static AccessViolationType DecodeArm64Access(uint64_t esr) {
	const uint64_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
	if (ec == ESR_EC_INSTRUCTION_ABORT_L || ec == ESR_EC_INSTRUCTION_ABORT) {
		return AccessViolationType::Execute;
	}
	return (esr & ESR_DATA_ABORT_WNR) != 0 ? AccessViolationType::Write : AccessViolationType::Read;
}

bool IsWriteFault(const void* native_context) {
	return DecodeArm64Access(FindFaultEsr(static_cast<const ucontext_t*>(native_context))) ==
	       AccessViolationType::Write;
}

// ARM64 host: faults here come from host code (e.g. GPU page tracking touching guest memory). Guest
// x86 register state lives in the guest CPU translator, so the x86 register fields stay zero.
static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	auto* context = static_cast<ucontext_t*>(native_context);
	RecordFaultRegisters(native_context);

	ExceptionInfo info {};
	info.exception_address = static_cast<uint64_t>(context->uc_mcontext.pc);
	info.native_code       = static_cast<uint32_t>(signal_number);
	info.native_context    = context;

	if (signal_number == SIGSEGV || signal_number == SIGBUS) {
		info.type                   = ExceptionType::AccessViolation;
		info.access_violation_type  = DecodeArm64Access(FindFaultEsr(context));
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	} else if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		ChainToDefault(signal_number);
		return;
	}

#if defined(KYTY_ANDROID)
    // This handler replaces the process SIGSEGV disposition after FEX has started.
    // Only host faults belong to Kyty's own native page tracker. If the faulting PC is
    // inside JIT-translated guest code, return directly to FEX's saved signal handler:
    // attempting to report it as an unrecoverable Kyty host fault kills the game before
    // FEX can repair the page, decode the instruction or apply guest address folding.
    if (signal_number == SIGSEGV) {
        Cpu::GuestCpu::GuestRegisters guest_regs {};
        if (Cpu::GuestCpu::CaptureFaultingGuestRegisters(native_context, &guest_regs)) {
            ChainToPrevious(signal_number, signal_info, native_context);
            return;
        }
    }
#endif
	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

#if defined(KYTY_ANDROID)
    ChainToPrevious(signal_number, signal_info, native_context);
#else
	ChainToDefault(signal_number);
#endif
}
#else
bool IsWriteFault(const void* native_context) {
	const auto error_code = static_cast<uint64_t>(
	    static_cast<const ucontext_t*>(native_context)->uc_mcontext.gregs[REG_ERR]);
	return (error_code & PAGE_FAULT_ERROR_INSTRUCTION) == 0 &&
	       (error_code & PAGE_FAULT_ERROR_WRITE) != 0;
}

static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	auto* context = static_cast<ucontext_t*>(native_context);
	auto* gregs   = context->uc_mcontext.gregs;

	ExceptionInfo info {};
	info.exception_address = static_cast<uint64_t>(gregs[REG_RIP]);
	info.native_code       = static_cast<uint32_t>(signal_number);
	info.native_context    = context;

	if (signal_number == SIGSEGV || signal_number == SIGBUS) {
		info.type             = ExceptionType::AccessViolation;
		const auto error_code = static_cast<uint64_t>(gregs[REG_ERR]);
		if ((error_code & PAGE_FAULT_ERROR_INSTRUCTION) != 0) {
			info.access_violation_type = AccessViolationType::Execute;
		} else if ((error_code & PAGE_FAULT_ERROR_WRITE) != 0) {
			info.access_violation_type = AccessViolationType::Write;
		} else {
			info.access_violation_type = AccessViolationType::Read;
		}
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	} else if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		ChainToDefault(signal_number);
		return;
	}

	info.rax = static_cast<uint64_t>(gregs[REG_RAX]);
	info.rbx = static_cast<uint64_t>(gregs[REG_RBX]);
	info.rcx = static_cast<uint64_t>(gregs[REG_RCX]);
	info.rdx = static_cast<uint64_t>(gregs[REG_RDX]);
	info.rsi = static_cast<uint64_t>(gregs[REG_RSI]);
	info.rdi = static_cast<uint64_t>(gregs[REG_RDI]);
	info.rbp = static_cast<uint64_t>(gregs[REG_RBP]);
	info.rsp = static_cast<uint64_t>(gregs[REG_RSP]);
	info.r8  = static_cast<uint64_t>(gregs[REG_R8]);
	info.r9  = static_cast<uint64_t>(gregs[REG_R9]);
	info.r10 = static_cast<uint64_t>(gregs[REG_R10]);
	info.r11 = static_cast<uint64_t>(gregs[REG_R11]);
	info.r12 = static_cast<uint64_t>(gregs[REG_R12]);
	info.r13 = static_cast<uint64_t>(gregs[REG_R13]);
	info.r14 = static_cast<uint64_t>(gregs[REG_R14]);
	info.r15 = static_cast<uint64_t>(gregs[REG_R15]);

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

	ChainToDefault(signal_number);
}
#endif // KYTY_HOST_ARM64

#endif

bool InstallHandler(Handler handler) {
	if (handler == nullptr) {
		return false;
	}

	uint32_t expected_state = 0;
	if (!g_install_state.compare_exchange_strong(expected_state, 1, std::memory_order_acq_rel)) {
		return expected_state == 2 && g_handler.load(std::memory_order_acquire) == handler;
	}

	g_handler.store(handler, std::memory_order_release);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (AddVectoredExceptionHandler(0, ExceptionFilter) == nullptr) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("AddVectoredExceptionHandler() failed\n");
		return false;
	}
#elif defined(__APPLE__)
	struct sigaction sa {};
	sa.sa_sigaction = SignalHandler;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	// The guest signal-dispatch path (KernelRaiseException) interrupts threads with
	// SIGUSR1; block it while a fault is being resolved so a stop-the-world request
	// cannot preempt the handler between the protection fix and the retry.
	sigaddset(&sa.sa_mask, SIGUSR1);

	// macOS raises SIGBUS for protection faults on some paths and SIGSEGV on others;
	// SIGILL covers instructions the host cannot execute (routed to the x64 emulator).
	bool ok = sigaction(SIGSEGV, &sa, nullptr) == 0 && sigaction(SIGBUS, &sa, nullptr) == 0 &&
	          sigaction(SIGILL, &sa, nullptr) == 0;
	if (!ok) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("sigaction() failed to install the host fault handler\n");
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;

	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
#if defined(KYTY_ANDROID) && KYTY_HOST_ARM64
        // Save the FEX/Android handlers before replacing them; otherwise a page fault
        // which Kyty cannot claim terminates the process instead of reaching FEX.
        auto* previous = PreviousSignalAction(signal_number);
        if (::sigaction(signal_number, nullptr, previous) != 0) {
            g_handler.store(nullptr, std::memory_order_release);
            g_install_state.store(0, std::memory_order_release);
            std::printf("sigaction(%d) could not query previous handler\n", signal_number);
            return false;
        }
#endif
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			g_handler.store(nullptr, std::memory_order_release);
			g_install_state.store(0, std::memory_order_release);
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
#endif

	g_install_state.store(2, std::memory_order_release);
	return true;
}

} // namespace Common::HostException

#if (defined(__linux__) || defined(__ANDROID__)) && defined(__aarch64__)
#include <ucontext.h>
#endif

namespace Common::HostException {

static thread_local unsigned long long t_fault_registers[3] {};

void RecordFaultRegisters(const void* native_context) {
#if (defined(__linux__) || defined(__ANDROID__)) && defined(__aarch64__)
	const auto* context  = static_cast<const ucontext_t*>(native_context);
	t_fault_registers[0] = context->uc_mcontext.pc;
	t_fault_registers[1] = context->uc_mcontext.regs[30];
	t_fault_registers[2] = context->uc_mcontext.regs[29];
#else
	(void)native_context;
#endif
}

void LastFaultRegisters(unsigned long long* pc, unsigned long long* lr, unsigned long long* fp) {
	*pc = t_fault_registers[0];
	*lr = t_fault_registers[1];
	*fp = t_fault_registers[2];
}

} // namespace Common::HostException
