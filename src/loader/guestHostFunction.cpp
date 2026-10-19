#include "loader/guestHostFunction.h"

#include "common/abi.h"

#if KYTY_HOST_ARM64
#include "cpu/guestCpu.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#endif

#include <atomic>

namespace Loader::GuestHost {

namespace {
std::atomic<ThreadTcbProvider> g_tcb_provider {nullptr};
} // namespace

void SetThreadTcbProvider(ThreadTcbProvider provider) {
	g_tcb_provider.store(provider, std::memory_order_release);
}

#if KYTY_HOST_ARM64
namespace {

struct TrapTable {
	std::mutex                                mutex;
	std::unordered_map<std::string, uint64_t> addresses; // key -> guest address
	std::unordered_map<uint32_t, std::string> labels;    // stub id -> label (nodes never move)
};

TrapTable& Traps() {
	static TrapTable table;
	return table;
}

// Thunk of a native function that has none: names it and stops, so the gap is visible at once.
void MissingThunk(GuestAbi::CallFrame& /*frame*/) {
	const std::string name(CurrentTrapName());
	std::fprintf(stderr, "ARM64: HLE %s has no guest ABI thunk\n", name.c_str());
	std::abort();
}

// Creates a trap. The caller holds the table mutex; no thunk runs during this call.
uint64_t MakeTrapLocked(TrapTable& table, std::string_view label, GuestAbi::HleThunk thunk) {
	const uint32_t id = Cpu::GuestCpu::AllocateHleId();
	table.labels.emplace(id, std::string(label));
	Cpu::GuestCpu::RegisterHle(id, thunk != nullptr ? thunk : &MissingThunk);
	return Cpu::GuestCpu::MakeHleStub(id);
}

constexpr uint64_t kAutoGuestStackSize = 2ull << 20;

// Guest stack owned by a host thread that was attached on first use; released at thread exit.
struct AutoGuestStack {
	uint64_t base = 0;

	~AutoGuestStack() {
		if (base != 0) {
			Cpu::GuestCpu::DetachCurrentThread();
			munmap(reinterpret_cast<void*>(base), kAutoGuestStackSize);
		}
	}
};

thread_local AutoGuestStack t_auto_stack;

} // namespace

uint64_t Address(std::string_view key, std::string_view label, uint64_t native_address,
                 GuestAbi::HleThunk thunk) {
	(void)native_address; // the thunk runs the native function; the guest only sees the trap
	auto&                         table = Traps();
	std::lock_guard               lock(table.mutex);
	const std::string             cache_key(key);
	if (const auto it = table.addresses.find(cache_key); it != table.addresses.end()) {
		return it->second;
	}
	const uint64_t address = MakeTrapLocked(table, label, thunk);
	table.addresses.emplace(cache_key, address);
	return address;
}

struct GuestNatives {
	std::mutex                                                     mutex;
	std::unordered_map<uint64_t, std::pair<GuestNativeFactory, uint64_t>> by_native; // -> (factory, code)
};

GuestNatives& Natives() {
	static GuestNatives natives;
	return natives;
}

void RegisterGuestNative(uint64_t native_address, GuestNativeFactory factory) {
	auto&           natives = Natives();
	std::lock_guard lock(natives.mutex);
	natives.by_native[native_address] = {factory, 0};
}

// Guest-native code registered for this native function, or 0.
static uint64_t GuestNativeAddress(uint64_t native_address) {
	auto&           natives = Natives();
	std::lock_guard lock(natives.mutex);
	const auto      it = natives.by_native.find(native_address);
	if (it == natives.by_native.end()) {
		return 0;
	}
	auto& [factory, code] = it->second;
	if (code == 0) {
		code = factory();
	}
	return code;
}

uint64_t SymbolAddress(const SymbolRecord& record) {
	if (!record.host_function || record.type != SymbolType::Func || record.vaddr == 0) {
		return record.vaddr;
	}
	if (const auto native = GuestNativeAddress(record.vaddr); native != 0) {
		return native;
	}
	const std::string_view label = record.dbg_name.empty() ? std::string_view(record.name)
	                                                       : std::string_view(record.dbg_name);
	return Address(record.name, label, record.vaddr, record.thunk);
}

std::string_view CurrentTrapName() {
	const uint32_t id = Cpu::GuestCpu::CurrentHleId();
	if (id == Cpu::GuestCpu::kNoHleId) {
		return {};
	}
	auto&           table = Traps();
	std::lock_guard lock(table.mutex);
	const auto      it = table.labels.find(id);
	return it != table.labels.end() ? std::string_view(it->second) : std::string_view {};
}

uint64_t NewTrap(std::string_view label, GuestAbi::HleThunk thunk) {
	auto&           table = Traps();
	std::lock_guard lock(table.mutex);
	return MakeTrapLocked(table, label, thunk);
}

bool AttachGuestThread(uint64_t stack_top) {
	if (!Cpu::GuestCpu::AttachCurrentThread(stack_top)) {
		return false;
	}
	if (const auto provider = g_tcb_provider.load(std::memory_order_acquire); provider != nullptr) {
		if (const uint64_t tcb = provider(); tcb != 0) {
			Cpu::GuestCpu::SetGuestFsBase(tcb);
		}
	}
	return true;
}

void EnsureGuestThread() {
	if (Cpu::GuestCpu::IsCurrentThreadAttached()) {
		return;
	}
	void* mem = mmap(nullptr, kAutoGuestStackSize, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		std::fprintf(stderr, "kyty guest cpu: failed to map a guest stack for a host thread\n");
		std::abort();
	}
	t_auto_stack.base = reinterpret_cast<uint64_t>(mem);
	if (!AttachGuestThread(t_auto_stack.base + kAutoGuestStackSize)) {
		std::fprintf(stderr, "kyty guest cpu: guest CPU attach failed on a host thread\n");
		std::abort();
	}
}

#else

// x86 hosts run guest code natively: addresses are native and attachment is not needed.
uint64_t Address(std::string_view /*key*/, std::string_view /*label*/, uint64_t native_address,
                 GuestAbi::HleThunk /*thunk*/) {
	return native_address;
}

uint64_t SymbolAddress(const SymbolRecord& record) {
	return record.vaddr;
}

std::string_view CurrentTrapName() {
	return {};
}

uint64_t NewTrap(std::string_view /*label*/, GuestAbi::HleThunk /*thunk*/) {
	std::fprintf(stderr, "NewTrap is an ARM64-only operation\n");
	std::abort();
}

void RegisterGuestNative(uint64_t /*native_address*/, GuestNativeFactory /*factory*/) {}

bool AttachGuestThread(uint64_t /*stack_top*/) {
	return true;
}

void EnsureGuestThread() {}

#endif

} // namespace Loader::GuestHost
