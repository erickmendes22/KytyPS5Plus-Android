// HLE trap path for the FEX backend: x86 stubs that enter the translator through syscall 400, and the
// CallFrame that exposes the trapped guest call to native thunks.

#include "cpu/guestCpuFex.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Cpu::GuestCpu {
namespace {

constexpr size_t kMaxHleIds  = 16384;
constexpr size_t kStubSize   = 20;
constexpr size_t kStackSlotOffset = 16; // [RSP] = id, [RSP+8] = return address, [RSP+16] = first stack arg

std::array<std::atomic<HleThunk>, kMaxHleIds> g_thunks {};
std::atomic<uint32_t>                         g_next_dynamic_id {kFirstDynamicHleId};
std::atomic<SafePointHook>                    g_safe_point_hook {nullptr};

// Stub layout (x86-64):
//   mov r10, rcx          49 89 CA        ; SYSCALL clobbers RCX, so the fourth argument moves to R10
//   push imm32 id         68 <id>         ; the id is read back from [RSP] by the handler
//   mov eax, 400          B8 90 01 00 00
//   syscall               0F 05
//   add rsp, 8            48 83 C4 08
//   ret                   C3
std::array<uint8_t, kStubSize> EncodeStub(uint32_t id) {
	std::array<uint8_t, kStubSize> code {0x49, 0x89, 0xCA, 0x68, 0, 0, 0, 0, 0xB8, 0x90, 0x01, 0x00, 0x00,
	                                     0x0F, 0x05, 0x48, 0x83, 0xC4, 0x08, 0xC3};
	for (size_t i = 0; i < 4; i++) {
		code[4 + i] = static_cast<uint8_t>(id >> (8 * i));
	}
	return code;
}

// Records the id of the trap being serviced, restoring the previous one on exit (thunks may re-enter).
class ScopedHleId {
public:
	explicit ScopedHleId(uint32_t id) : previous_(Cpu::GuestCpu::Fex::g_hle_id) {
		Cpu::GuestCpu::Fex::g_hle_id = id;
	}
	~ScopedHleId() {
		Cpu::GuestCpu::Fex::g_hle_id = previous_;
	}
	ScopedHleId(const ScopedHleId&)            = delete;
	ScopedHleId& operator=(const ScopedHleId&) = delete;

private:
	uint32_t previous_;
};

// Saves the trapped frame as the innermost one for CallGuest, restoring the previous on exit.
class ScopedHleFrame {
public:
	explicit ScopedHleFrame(FEXCore::Core::CpuStateFrame* frame) : previous_(Cpu::GuestCpu::Fex::g_hle_frame) {
		Cpu::GuestCpu::Fex::g_hle_frame = frame;
	}
	~ScopedHleFrame() {
		Cpu::GuestCpu::Fex::g_hle_frame = previous_;
	}
	ScopedHleFrame(const ScopedHleFrame&)            = delete;
	ScopedHleFrame& operator=(const ScopedHleFrame&) = delete;

private:
	FEXCore::Core::CpuStateFrame* previous_;
};

// Low 128 bits of XMM reg. The register file stride depends on whether FEX keeps 256-bit slots.
uint64_t* XmmWords(FEXCore::Core::CpuStateFrame* frame, int reg, bool sve256) {
	auto* base = sve256 ? &frame->State.xmm.avx.data[reg][0] : &frame->State.xmm.sse.data[reg][0];
	return base;
}

class FexCallFrame final : public Loader::GuestAbi::CallFrame {
public:
	FexCallFrame(FEXCore::Core::CpuStateFrame* frame, const std::array<uint64_t, 6>& int_args, uint64_t stack_args,
	             bool sve256)
	    : frame_(frame), int_args_(int_args), stack_args_(stack_args), sve256_(sve256) {}

	uint64_t IntArg(int index) const override {
		return int_args_.at(static_cast<size_t>(index));
	}

	Loader::GuestAbi::Xmm VecArg(int index) const override {
		const uint64_t* words = XmmWords(frame_, index, sve256_);
		return {words[0], words[1]};
	}

	uint64_t StackArg(int index) const override {
		uint64_t value = 0;
		std::memcpy(&value, reinterpret_cast<const void*>(stack_args_ + 8 * static_cast<uint64_t>(index)), sizeof(value));
		return value;
	}

	uint64_t StackArgAddress(int index) const override {
		return stack_args_ + 8 * static_cast<uint64_t>(index);
	}

	void SetIntReturn(uint64_t rax) override {
		rax_ = rax;
	}

	void SetIntReturn2(uint64_t rdx) override {
		frame_->State.gregs[FEXCore::X86State::REG_RDX] = rdx;
	}

	void SetVecReturn(Loader::GuestAbi::Xmm xmm0) override {
		uint64_t* words = XmmWords(frame_, 0, sve256_);
		words[0]        = xmm0.lo;
		words[1]        = xmm0.hi;
	}

	// Value the syscall handler returns; FEX writes it to RAX.
	uint64_t IntReturn() const {
		return rax_;
	}

private:
	FEXCore::Core::CpuStateFrame*    frame_;
	std::array<uint64_t, 6>          int_args_;
	uint64_t                         stack_args_;
	bool                             sve256_;
	uint64_t                         rax_ = 0;
};

} // namespace

namespace Fex {

HleThunk LookupHle(uint32_t id) {
	if (id >= kMaxHleIds) {
		return nullptr;
	}
	return g_thunks[id].load(std::memory_order_acquire);
}

uint64_t HleSyscall(FEXCore::Core::CpuStateFrame* frame, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5) {
	// Syscall arguments arrive as RDI, RSI, RDX, R10 (the stub's copy of RCX), R8, R9.
	const uint64_t guest_rsp = frame->State.gregs[FEXCore::X86State::REG_RSP];
	uint64_t       id_slot   = 0;
	std::memcpy(&id_slot, reinterpret_cast<const void*>(guest_rsp), sizeof(id_slot));
	const auto id = static_cast<uint32_t>(id_slot);

	const HleThunk thunk = LookupHle(id);
	if (thunk == nullptr) {
		std::fprintf(stderr, "kyty guest cpu: no HLE thunk registered for stub id %u\n", id);
		return static_cast<uint64_t>(-static_cast<int64_t>(ENOSYS));
	}

	const bool sve256 = Cpu::GuestCpu::Fex::GetRuntime().sve256;
	ScopedHleId    id_scope(id);
	ScopedHleFrame scope(frame);
	FexCallFrame   call_frame(frame, {arg0, arg1, arg2, arg3, arg4, arg5}, guest_rsp + kStackSlotOffset, sve256);
	thunk(call_frame);
	// Safe point: the trapped guest state is consistent and the thunk is done with it.
	if (auto* hook = g_safe_point_hook.load(std::memory_order_acquire); hook != nullptr) {
		hook();
	}
	return call_frame.IntReturn();
}

} // namespace Fex

uint64_t MakeHleStub(uint32_t id) {
	const auto code = EncodeStub(id);
	return AllocateGuestCode(code);
}

uint32_t AllocateHleId() {
	const uint32_t id = g_next_dynamic_id.fetch_add(1, std::memory_order_relaxed);
	if (id >= kMaxHleIds) {
		std::fprintf(stderr, "kyty guest cpu: HLE stub id space exhausted\n");
		std::abort();
	}
	return id;
}

bool CaptureTrappedGuestRegisters(GuestRegisters* out) {
	const auto* frame = Fex::g_hle_frame;
	if (frame == nullptr || out == nullptr) {
		return false;
	}
	static_assert(sizeof(out->gpr) == sizeof(frame->State.gregs));
	std::memcpy(out->gpr, frame->State.gregs, sizeof(out->gpr));
	// The stub left [RSP] = id and [RSP + 8] = return address (see EncodeStub).
	const uint64_t trap_rsp = frame->State.gregs[FEXCore::X86State::REG_RSP];
	std::memcpy(&out->rip, reinterpret_cast<const void*>(trap_rsp + 8), sizeof(out->rip));
	out->gpr[GuestRegisters::kRsp] = trap_rsp + kStackSlotOffset;
	out->fs_base                   = frame->State.fs_cached;
	return true;
}

void RegisterSafePointHook(SafePointHook hook) {
	g_safe_point_hook.store(hook, std::memory_order_release);
}

uint32_t CurrentHleId() {
	return Fex::g_hle_id;
}

void RegisterHle(uint32_t id, HleThunk thunk) {
	if (id >= kMaxHleIds) {
		std::fprintf(stderr, "kyty guest cpu: HLE id %u exceeds the table size\n", id);
		std::abort();
	}
	g_thunks[id].store(thunk, std::memory_order_release);
}

} // namespace Cpu::GuestCpu
