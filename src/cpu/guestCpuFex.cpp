// FEXCore backend for Cpu::GuestCpu (ARM64 hosts). Bring-up mirrors Source/Tools/FEXInterpreter
// minus the ELF loader, so it runs without a RootFS: guest code here is raw x86-64 bytes.

#include "cpu/guestCpuFex.h"

#include "CodeLoader.h"
#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include "PortabilityInfo.h"
#include "VDSO_Emulation.h"
#include "FEXHeaderUtils/Syscalls.h"
#include "LinuxSyscalls/Utils/Threads.h"
#include "LinuxSyscalls/x64/Syscalls.h"

#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/PrctlUtils.h>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

extern char** environ;

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Cpu::GuestCpu {
namespace {

using Fex::Runtime;

constexpr uint64_t kGuestCodeChunkSize = 64 * 1024;
constexpr uint64_t kRedZoneSkip        = 256; // SysV red zone below an interrupted RSP
constexpr uint64_t kEntryStackSlack    = 64;  // headroom below a stack top before the first call

Runtime g_runtime {};

std::atomic<HostFaultHandler> g_fault_handler {nullptr};

struct AttachedGuestThread {
	FEX::HLE::ThreadStateObject* state     = nullptr;
	uint64_t                     stack_top = 0;
};
thread_local AttachedGuestThread t_attached {};

// Bump arena for guest code. Chunks are RWX and never remapped, so a stub that has been handed out
// is never written again and can be fetched by the translator at any time.
std::mutex g_code_mutex;
uint64_t   g_code_cursor = 0;
uint64_t   g_code_end    = 0;

[[noreturn]] void Die(const char* message) {
	std::fprintf(stderr, "kyty guest cpu: %s\n", message);
	std::abort();
}

// CPUState is non-copyable; save only what a re-entrant guest call must preserve.
struct SavedGuestRegs {
	uint64_t                               rip {};
	uint64_t                               gregs[16] {};
	uint64_t                               avx_high[16][2] {};
	FEXCore::Core::CPUState::XMMRegs       xmm {};
	uint8_t                                flags[48] {};
	uint32_t                               pf_raw {}, af_raw {}, mxcsr {};

	void Save(const FEXCore::Core::CPUState& s) {
		rip = s.rip;
		std::memcpy(gregs, s.gregs, sizeof(gregs));
		std::memcpy(avx_high, s.avx_high, sizeof(avx_high));
		std::memcpy(&xmm, &s.xmm, sizeof(xmm));
		std::memcpy(flags, s.flags, sizeof(flags));
		pf_raw = s.pf_raw;
		af_raw = s.af_raw;
		mxcsr  = s.mxcsr;
	}

	void Restore(FEXCore::Core::CPUState& s) const {
		s.rip = rip;
		std::memcpy(s.gregs, gregs, sizeof(gregs));
		std::memcpy(s.avx_high, avx_high, sizeof(avx_high));
		std::memcpy(&s.xmm, &xmm, sizeof(xmm));
		std::memcpy(s.flags, flags, sizeof(flags));
		s.pf_raw = pf_raw;
		s.af_raw = af_raw;
		s.mxcsr  = mxcsr;
	}
};

// FEX delivers Kyty's SIGSEGV hook through the signal delegator, before its own handling.
bool HostSigsegv(FEXCore::Core::InternalThreadState* /*thread*/, int /*sig*/, void* info, void* /*ucontext*/) {
	auto* handler = g_fault_handler.load(std::memory_order_acquire);
	if (handler == nullptr) {
		return false;
	}
	const auto* si = static_cast<const siginfo_t*>(info);
	return handler(reinterpret_cast<uintptr_t>(si->si_addr));
}

void FexMsgHandler(LogMan::DebugLevels /*level*/, const char* message) {
	std::fputs(message, stderr);
}

void FexAssertHandler(const char* message) {
	std::fprintf(stderr, "FEX assert: %s\n", message);
	std::abort();
}

class KytyCodeLoader final : public FEX::CodeLoader {
public:
	// Kyty owns guest stacks and entry points; the translator's loader hooks are not used.
	uint64_t StackSize() const override {
		return 8ull << 20;
	}
	uint64_t GetStackPointer() const override {
		return 0;
	}
	uint64_t DefaultRIP() const override {
		return 0;
	}
};

void EnableHardwareTsoIfAvailable(FEXCore::Context::Context* ctx) {
	const int result = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
	if (result == -1) {
		return; // Kernel has no memory-model control; FEX falls back to software TSO.
	}
	FEX_CONFIG_OPT(TSOEnabled, TSOENABLED);
	if (!TSOEnabled()) {
		return;
	}
	if (result == PR_SET_MEM_MODEL_DEFAULT && prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0) == 0) {
		ctx->SetHardwareTSOSupport(true);
	}
}

void SetupTranslatorConfig() {
	// Same layer setup as FEXInterpreter: directories, meta layer, then the standard FEX config files
	// and FEX_* environment overrides. Program name only selects per-app config sections.
	FEX::Config::LoadConfig("kyty-guest", environ, FEX::ReadPortabilityInformation());
	FEXCore::Config::ReloadMetaLayer();
	FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
	FEXCore::Config::Set(FEXCore::Config::CONFIG_APP_FILENAME, "kyty-guest");
	FEXCore::Config::Set(FEXCore::Config::CONFIG_APP_CONFIG_NAME, "kyty-guest");
	FEXCore::Config::Set(FEXCore::Config::CONFIG_INTERPRETER_INSTALLED, "0");
}

bool BringUpTranslator() {
	LogMan::Msg::InstallHandler(FexMsgHandler);
	LogMan::Throw::InstallHandler(FexAssertHandler);

	SetupTranslatorConfig();

	// Owned for the life of the process; FEX keeps raw pointers to these objects.
	static auto thread_handlers = FEX::LinuxEmulation::Threads::SetupThreadHandlers();
	static KytyCodeLoader loader;

	const auto host = FEX::FetchHostFeatures();
	g_runtime.sve256 = host.SupportsSVE256;

	static auto ctx = FEXCore::Context::Context::CreateNewContext(host);
	EnableHardwareTsoIfAvailable(ctx.get());

	static auto signals = FEX::HLE::CreateSignalDelegator(ctx.get(), "kyty-guest", host.SupportsAVX, host.SupportsSVE256);
	static auto thunks  = FEX::HLE::CreateThunkHandler();
	static auto syscalls = FEX::HLE::x64::CreateHandler(ctx.get(), signals.get(), thunks.get());

	syscalls->SetCodeLoader(&loader);
	ctx->SetSignalDelegator(signals.get());
	ctx->SetSyscallHandler(syscalls.get());
	ctx->SetThunkHandler(thunks.get());

	FEX::HLE::x64::RegisterSyscall(syscalls.get(), Fex::kHleSyscallNumber, "kyty_hle", Fex::HleSyscall);

	if (!ctx->InitCore()) {
		std::fprintf(stderr, "kyty guest cpu: FEXCore failed to initialise\n");
		return false;
	}

	// The VDSO carries FEX's callback return trampoline (GetThunkCallbackRET). FEXInterpreter maps it
	// before any guest code runs; without it every host->guest callback returns to address 0.
	auto* init_thread = syscalls->TM.CreateThread();
	syscalls->TM.TrackThread(init_thread);
	signals->RegisterTLSState(init_thread);
	thunks->RegisterTLSState(init_thread);
	static auto vdso = FEX::VDSO::LoadVDSOThunks(init_thread->Thread, true, syscalls.get());
	thunks->AppendThunkDefinitions(FEX::VDSO::GetVDSOThunkDefinitions(true));
	signals->SetVDSOSymbols();
	signals->UninstallTLSState(init_thread);
	syscalls->TM.DestroyThread(init_thread);

	g_runtime.ctx      = ctx.get();
	g_runtime.signals  = signals.get();
	g_runtime.syscalls = syscalls.get();
	g_runtime.thunks   = thunks.get();
	return true;
}

// Frame the current call should run on: the trapped guest frame inside an HLE thunk, otherwise the
// attached thread's own frame.
FEXCore::Core::CpuStateFrame* CurrentFrame() {
	if (Fex::g_hle_frame != nullptr) {
		return Fex::g_hle_frame;
	}
	if (t_attached.state == nullptr) {
		Die("guest call on a host thread that is not attached");
	}
	return t_attached.state->Thread->CurrentFrame;
}

uint64_t RunGuest(FEXCore::Core::CpuStateFrame* frame, uint64_t stack_base, uint64_t entry,
                  std::span<const uint64_t> int_args) {
	if (int_args.size() > kMaxCallArgs) {
		Die("CallGuest supports at most 6 integer arguments");
	}

	SavedGuestRegs saved;
	saved.Save(frame->State);

	auto& gregs = frame->State.gregs;
	// HandleCallback pushes its return trampoline below RSP; start 8 mod 16 so the callee sees a normal
	// post-call alignment.
	gregs[FEXCore::X86State::REG_RSP] = (stack_base & ~uint64_t {15}) - 8;

	constexpr uint32_t kArgRegs[kMaxCallArgs] = {
	    FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI, FEXCore::X86State::REG_RDX,
	    FEXCore::X86State::REG_RCX, FEXCore::X86State::REG_R8,  FEXCore::X86State::REG_R9};
	for (size_t i = 0; i < int_args.size(); i++) {
		gregs[kArgRegs[i]] = int_args[i];
	}

	g_runtime.ctx->HandleCallback(frame->Thread, entry);

	const uint64_t result = gregs[FEXCore::X86State::REG_RAX];
	saved.Restore(frame->State);
	return result;
}

} // namespace

namespace Fex {

Runtime& GetRuntime() {
	return g_runtime;
}

} // namespace Fex

bool Init() {
	static const bool ok = BringUpTranslator();
	return ok;
}

bool AttachCurrentThread(uint64_t guest_stack_top) {
	if (!Init()) {
		return false;
	}
	if (t_attached.state != nullptr) {
		t_attached.stack_top = guest_stack_top;
		return true;
	}

	auto& rt = Fex::GetRuntime();
	auto* state = rt.syscalls->TM.CreateThread();
	rt.syscalls->TM.TrackThread(state);
	rt.signals->RegisterTLSState(state);
	rt.syscalls->RegisterTLSState(state);
	rt.thunks->RegisterTLSState(state);

	state->ThreadInfo.PID = ::getpid();
	state->ThreadInfo.TID = FHU::Syscalls::gettid();
	FEXCore::Allocator::InitializeThread();

	// ExecuteThread normally installs this; HandleCallback on its own does not.
	state->Thread->CurrentFrame->Pointers.ThunkCallbackRet = rt.signals->GetThunkCallbackRET();

	t_attached = {state, guest_stack_top};
	return true;
}

void DetachCurrentThread() {
	if (t_attached.state == nullptr) {
		return;
	}
	auto& rt = Fex::GetRuntime();
	rt.signals->UninstallTLSState(t_attached.state);
	rt.syscalls->UninstallTLSState(t_attached.state);
	rt.syscalls->TM.DestroyThread(t_attached.state);
	t_attached = {};
}

bool IsCurrentThreadAttached() {
	return t_attached.state != nullptr;
}

void SetGuestFsBase(uint64_t base) {
	CurrentFrame()->State.fs_cached = base;
}

uint64_t CallGuest(uint64_t entry, std::span<const uint64_t> int_args) {
	if (Fex::g_hle_frame != nullptr) {
		auto* frame = Fex::g_hle_frame;
		return RunGuest(frame, frame->State.gregs[FEXCore::X86State::REG_RSP] - kRedZoneSkip, entry, int_args);
	}
	if (t_attached.state == nullptr) {
		Die("CallGuest on a host thread that is not attached");
	}
	return RunGuest(CurrentFrame(), t_attached.stack_top - kEntryStackSlack, entry, int_args);
}

uint64_t CallGuestOnStack(uint64_t stack_top, uint64_t entry, std::span<const uint64_t> int_args) {
	return RunGuest(CurrentFrame(), stack_top - kEntryStackSlack, entry, int_args);
}

uint64_t AllocateGuestCode(std::span<const uint8_t> code) {
	if (!Init()) {
		Die("AllocateGuestCode before translator init");
	}
	std::lock_guard lock(g_code_mutex);
	const uint64_t size = (code.size() + 15) & ~uint64_t {15};
	if (g_code_cursor == 0 || g_code_cursor + size > g_code_end) {
		const uint64_t chunk = size > kGuestCodeChunkSize ? size : kGuestCodeChunkSize;
		void* mem = mmap(nullptr, chunk, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mem == MAP_FAILED) {
			Die("failed to map guest code memory");
		}
		// FEX only executes addresses its VMA table knows about; host mmaps bypass the syscall layer.
		auto* state = t_attached.state;
		Fex::GetRuntime().syscalls->AddVirtualPage(state != nullptr ? state->Thread : nullptr,
		                                           reinterpret_cast<uint64_t>(mem), chunk,
		                                           PROT_READ | PROT_WRITE | PROT_EXEC);
		g_code_cursor = reinterpret_cast<uint64_t>(mem);
		g_code_end    = g_code_cursor + chunk;
	}
	const uint64_t address = g_code_cursor;
	std::memcpy(reinterpret_cast<void*>(address), code.data(), code.size());
	g_code_cursor += size;
	return address;
}

void RegisterHostFaultHandler(HostFaultHandler handler) {
	if (!Init()) {
		Die("RegisterHostFaultHandler before translator init");
	}
	static std::once_flag once;
	g_fault_handler.store(handler, std::memory_order_release);
	std::call_once(once, [] {
		Fex::GetRuntime().signals->RegisterHostSignalHandler(SIGSEGV, HostSigsegv, true);
	});
}

} // namespace Cpu::GuestCpu
