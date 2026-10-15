// In-process guest CPU self-test. Hand-assembled x86-64 code runs through the same entry, HLE and
// fault paths as a game would, so it also exercises executable-memory rules for the host process.
// Mirrors tests/GuestCpuFexTests.cpp (T1, T3, T5) with failures reported instead of aborting.

#include "cpu/guestCpuSelfTest.h"

#include "cpu/guestCpu.h"

#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <vector>

namespace Cpu::GuestCpu {
namespace {

using Loader::GuestAbi::CallFrame;

enum : uint32_t { kAdd5 = 1, kCallback = 2 };

constexpr uint64_t kStackSize = 1u << 20;
constexpr size_t   kPageSize  = 4096;

// Little-endian x86 byte builder.
class X86Code {
public:
	X86Code& Op(std::initializer_list<uint8_t> bytes) {
		bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
		return *this;
	}
	X86Code& Imm32(uint32_t value) {
		for (int i = 0; i < 4; i++) {
			bytes_.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}
		return *this;
	}
	X86Code& Imm64(uint64_t value) {
		for (int i = 0; i < 8; i++) {
			bytes_.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}
		return *this;
	}
	X86Code& MovEdiImm(uint32_t v) { return Op({0xBF}).Imm32(v); }
	X86Code& MovEsiImm(uint32_t v) { return Op({0xBE}).Imm32(v); }
	X86Code& MovEdxImm(uint32_t v) { return Op({0xBA}).Imm32(v); }
	X86Code& MovEcxImm(uint32_t v) { return Op({0xB9}).Imm32(v); }
	X86Code& MovR8dImm(uint32_t v) { return Op({0x41, 0xB8}).Imm32(v); }
	X86Code& MovRdiImm64(uint64_t v) { return Op({0x48, 0xBF}).Imm64(v); }
	X86Code& MovRaxImm64(uint64_t v) { return Op({0x48, 0xB8}).Imm64(v); }
	X86Code& CallRax() { return Op({0xFF, 0xD0}); }
	X86Code& Ret() { return Op({0xC3}); }

	uint64_t Install() const { return AllocateGuestCode(bytes_); }

private:
	std::vector<uint8_t> bytes_;
};

struct Tally {
	int passed = 0;
	int failed = 0;
};

void Report(Tally& tally, const char* name, uint64_t got, uint64_t want) {
	if (got == want) {
		tally.passed++;
		std::printf("FEX-SELFTEST PASS %s\n", name);
	} else {
		tally.failed++;
		std::printf("FEX-SELFTEST FAIL %s: got=0x%llx want=0x%llx\n", name,
		            static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
	}
	std::fflush(stdout);
}

// T1: integer arguments RDI..R9 reach native code; the sum is returned in RAX.
void Add5Thunk(CallFrame& frame) {
	uint64_t sum = 0;
	for (int i = 0; i < 5; i++) {
		sum += frame.IntArg(i);
	}
	frame.SetIntReturn(sum);
}

// T3: native code calls back into guest code mid-HLE and doubles the result.
void CallbackThunk(CallFrame& frame) {
	const auto     fn    = frame.IntArg(0);
	const auto     arg   = frame.IntArg(1);
	const uint64_t value = CallGuest(fn, std::span<const uint64_t>(&arg, 1));
	frame.SetIntReturn(2 * value);
}

// T5 state: the guard page is made writable by the handler on its first write fault.
void*            g_guard_page   = nullptr;
std::atomic<int> g_guard_faults {0};

bool GuardFault(uintptr_t address) {
	const auto page = reinterpret_cast<uintptr_t>(g_guard_page);
	if (g_guard_page == nullptr || address < page || address >= page + kPageSize) {
		return false;
	}
	mprotect(g_guard_page, kPageSize, PROT_READ | PROT_WRITE);
	g_guard_faults.fetch_add(1);
	return true;
}

uint64_t MapStack() {
	void* mem = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		return 0;
	}
	return reinterpret_cast<uint64_t>(mem) + kStackSize;
}

void TestGuestAdd(Tally& tally) {
	// lea rax, [rdi + rsi] ; ret
	const uint64_t add  = X86Code().Op({0x48, 0x8D, 0x04, 0x37}).Ret().Install();
	const uint64_t args[2] = {20, 22};
	Report(tally, "T0 guest add", CallGuest(add, args), 42);
}

void TestHleIntegerArgs(Tally& tally) {
	const uint64_t caller = X86Code()
	                            .MovEdiImm(1)
	                            .MovEsiImm(20)
	                            .MovEdxImm(300)
	                            .MovEcxImm(4000)
	                            .MovR8dImm(50000)
	                            .MovRaxImm64(MakeHleStub(kAdd5))
	                            .CallRax()
	                            .Ret()
	                            .Install();
	Report(tally, "T1 hle int args", CallGuest(caller, {}), 54321);
}

void TestHostToGuestCallback(Tally& tally) {
	// mov rax, rdi ; imul rax, rax ; add rax, 7 ; ret
	const uint64_t square_plus = X86Code()
	                                 .Op({0x48, 0x89, 0xF8})
	                                 .Op({0x48, 0x0F, 0xAF, 0xC0})
	                                 .Op({0x48, 0x83, 0xC0, 0x07})
	                                 .Ret()
	                                 .Install();
	const uint64_t caller = X86Code()
	                            .MovRdiImm64(square_plus)
	                            .MovEsiImm(9)
	                            .MovRaxImm64(MakeHleStub(kCallback))
	                            .CallRax()
	                            .Ret()
	                            .Install();
	Report(tally, "T3 host->guest callback", CallGuest(caller, {}), 2 * (81 + 7));
}

void TestHostFaultHandler(Tally& tally) {
	void* page = mmap(nullptr, kPageSize, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		tally.failed++;
		std::printf("FEX-SELFTEST FAIL T5 guard page mmap\n");
		return;
	}
	g_guard_page = page;
	RegisterHostFaultHandler(GuardFault);

	// mov qword ptr [rdi + 24], 0xABCDEF ; ret
	const uint64_t store   = X86Code().Op({0x48, 0xC7, 0x47, 0x18}).Imm32(0xABCDEF).Ret().Install();
	const uint64_t args[1] = {reinterpret_cast<uint64_t>(page)};
	CallGuest(store, args);

	const uint64_t stored = reinterpret_cast<volatile uint64_t*>(page)[3];
	Report(tally, "T5 write fault via host handler", stored == 0xABCDEF ? 1 : 0, 1);
	Report(tally, "T5 fault handler ran once", static_cast<uint64_t>(g_guard_faults.load()), 1);
}

} // namespace

bool RunGuestCpuSelfTest() {
	Tally tally;

	if (!Init()) {
		std::printf("FEX-SELFTEST FAIL translator init\n");
		return false;
	}
	std::printf("FEX-SELFTEST translator init ok\n");

	const uint64_t stack = MapStack();
	if (stack == 0 || !AttachCurrentThread(stack)) {
		std::printf("FEX-SELFTEST FAIL attach main thread\n");
		return false;
	}

	RegisterHle(kAdd5, Add5Thunk);
	RegisterHle(kCallback, CallbackThunk);

	TestGuestAdd(tally);
	TestHleIntegerArgs(tally);
	TestHostToGuestCallback(tally);
	TestHostFaultHandler(tally);

	DetachCurrentThread();

	std::printf("FEX-SELFTEST DONE passed=%d failed=%d\n", tally.passed, tally.failed);
	std::fflush(stdout);
	return tally.failed == 0;
}

} // namespace Cpu::GuestCpu
