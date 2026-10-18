// ARM64-only: exercises the FEXCore guest CPU backend with hand-assembled x86-64 code in executable
// memory (no guest ELF, no RootFS). Ported from the phase-0 spike: T1, T3, T4, T5, T7.

#include "cpu/guestCpu.h"
#include "libs/libs.h"
#include "loader/guestCall.h"
#include "loader/guestHostFunction.h"
#include "loader/symbolDatabase.h"

#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace Cpu::GuestCpu;
using Loader::GuestAbi::CallFrame;

void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "GuestCpuFexTests: failed: %s\n", message);
    std::abort();
  }
}

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

  uint64_t Install() const {
    return AllocateGuestCode(bytes_);
  }

private:
  std::vector<uint8_t> bytes_;
};

uint64_t MakeStub(uint32_t id, HleThunk thunk) {
  RegisterHle(id, thunk);
  return MakeHleStub(id);
}

// --- Thunks ---------------------------------------------------------------------------------------

enum : uint32_t { kAdd5 = 1, kCallback = 2, kSetFs = 3, kGuardPage = 4 };

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
  const auto fn  = frame.IntArg(0);
  const auto arg = frame.IntArg(1);
  const uint64_t value = CallGuest(fn, std::span<const uint64_t>(&arg, 1));
  frame.SetIntReturn(2 * value);
}

// T4: host sets the guest FS base for the current thread.
void SetFsThunk(CallFrame& frame) {
  SetGuestFsBase(frame.IntArg(0));
  frame.SetIntReturn(0);
}

// Native function with a guest callback argument. The callback runs through the production helper.
int DoubleViaGuestCallback(int (*fn)(int), int value) {
  return 2 * Loader::GuestHost::CallGuestFunction(fn, value);
}

// --- Guest-visible state --------------------------------------------------------------------------

uint64_t g_tls_block[4] = {0, 0, 0xC0FFEE1234ull, 0};

std::atomic<int> g_guard_faults {0};
std::atomic<int> g_guard_writes {0};
void*            g_guard_page  = nullptr;

bool GuardFault(uintptr_t address, bool is_write) {
  const auto page = reinterpret_cast<uintptr_t>(g_guard_page);
  if (g_guard_page == nullptr || address < page || address >= page + 4096) {
    return false;
  }
  mprotect(g_guard_page, 4096, PROT_READ | PROT_WRITE);
  g_guard_faults.fetch_add(1);
  g_guard_writes.fetch_add(is_write ? 1 : 0);
  return true;
}

// T7: guest counter loop. RDI = iterations, RSI = counter address.
//   loop: lock add qword ptr [rsi], 1 ; dec rdi ; jnz loop ; mov eax, 0x5157 ; ret
uint64_t MakeCounterLoop() {
  return X86Code()
      .Op({0xF0, 0x48, 0x83, 0x06, 0x01})
      .Op({0x48, 0xFF, 0xCF})
      .Op({0x75, 0xF6})
      .Op({0xB8, 0x57, 0x51, 0x00, 0x00})
      .Ret()
      .Install();
}

constexpr uint64_t kStackSize = 1u << 20;

uint64_t MapStack() {
  void* mem = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(mem != MAP_FAILED, "stack mmap");
  return reinterpret_cast<uint64_t>(mem) + kStackSize;
}

// --- Tests ----------------------------------------------------------------------------------------

void TestHleIntegerArgs(uint64_t add5_stub) {
  const uint64_t caller = X86Code()
                              .MovEdiImm(1)
                              .MovEsiImm(20)
                              .MovEdxImm(300)
                              .MovEcxImm(4000)
                              .MovR8dImm(50000)
                              .MovRaxImm64(add5_stub)
                              .CallRax()
                              .Ret()
                              .Install();
  Check(CallGuest(caller, {}) == 54321, "T1 hle int args");
  std::puts("PASS T1 hle int args");
}

void TestHostToGuestCallback(uint64_t callback_stub) {
  const uint64_t square_plus = X86Code()
                                   .Op({0x48, 0x89, 0xF8})       // mov rax, rdi
                                   .Op({0x48, 0x0F, 0xAF, 0xC0}) // imul rax, rax
                                   .Op({0x48, 0x83, 0xC0, 0x07}) // add rax, 7
                                   .Ret()
                                   .Install();
  const uint64_t caller = X86Code()
                              .MovRdiImm64(square_plus)
                              .MovEsiImm(9)
                              .MovRaxImm64(callback_stub)
                              .CallRax()
                              .Ret()
                              .Install();
  Check(CallGuest(caller, {}) == 2 * (81 + 7), "T3 host->guest callback");
  std::puts("PASS T3 host->guest callback");
}

void TestFsBase(uint64_t set_fs_stub) {
  const uint64_t caller = X86Code()
                              .MovRdiImm64(reinterpret_cast<uint64_t>(g_tls_block))
                              .MovRaxImm64(set_fs_stub)
                              .CallRax()
                              // mov rax, qword ptr fs:[0x10]
                              .Op({0x64, 0x48, 0x8B, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00})
                              .Ret()
                              .Install();
  Check(CallGuest(caller, {}) == 0xC0FFEE1234ull, "T4 fs base set by host");
  std::puts("PASS T4 fs base set by host");
}

void TestHostFaultHandler() {
  void* page = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(page != MAP_FAILED, "guard page mmap");
  g_guard_page = page;
  RegisterHostFaultHandler(GuardFault);

  // mov qword ptr [rdi + 24], 0xABCDEF ; ret
  const uint64_t store = X86Code().Op({0x48, 0xC7, 0x47, 0x18}).Imm32(0xABCDEF).Ret().Install();
  const uint64_t args[1] = {reinterpret_cast<uint64_t>(page)};
  CallGuest(store, args);

  const bool ok = reinterpret_cast<volatile uint64_t*>(page)[3] == 0xABCDEF && g_guard_faults.load() == 1 &&
                  g_guard_writes.load() == 1;
  Check(ok, "T5 host fault handler first (GPU page tracking)");
  std::puts("PASS T5 host fault handler first (GPU page tracking)");
}

void TestHostCreatedGuestThread() {
  constexpr uint64_t kIters = 2000000;
  static uint64_t    counter = 0;
  counter = 0;
  const uint64_t loop = MakeCounterLoop();

  uint64_t worker_result = 0;
  std::thread worker([&] {
    Check(AttachCurrentThread(MapStack()), "attach worker");
    const uint64_t args[2] = {kIters, reinterpret_cast<uint64_t>(&counter)};
    worker_result = CallGuest(loop, args);
    DetachCurrentThread();
  });

  Check(AttachCurrentThread(MapStack()), "attach main");
  const uint64_t args[2] = {kIters, reinterpret_cast<uint64_t>(&counter)};
  const uint64_t main_result = CallGuest(loop, args);
  worker.join();
  DetachCurrentThread();

  Check(worker_result == 0x5157 && main_result == 0x5157, "T7 host-spawned guest thread return");
  Check(__atomic_load_n(&counter, __ATOMIC_SEQ_CST) == 2 * kIters, "T7 guest atomics under TSO");
  std::puts("PASS T7 host-spawned guest thread + atomics");
}

} // namespace

// Resolves a real libC function from the import database, as a relocation would, and calls it from
// translated x86 code through the trap. Also checks that a data symbol keeps its host address.
void TestImportedHleFunction() {
  Loader::SymbolDatabase db;
  Libs::InitAll(&db);

  const auto* strlen_rec = db.FindByNid("j4ViWNHEgww", Loader::SymbolType::Func);
  Check(strlen_rec != nullptr, "libC strlen record present");
  Check(strlen_rec->host_function && strlen_rec->thunk != nullptr, "strlen is an HLE function with a thunk");
  const uint64_t guest_strlen = Loader::GuestHost::SymbolAddress(*strlen_rec);
  Check(guest_strlen != strlen_rec->vaddr, "guest sees a trap, not the host address");

  const auto* guard_rec = db.FindByNid("f7uOxY9mM1U", Loader::SymbolType::Object);
  Check(guard_rec != nullptr && Loader::GuestHost::SymbolAddress(*guard_rec) == guard_rec->vaddr,
        "data symbol keeps its host address");

  static const char text[] = "astro bot";
  const uint64_t caller = X86Code()
                              .MovRdiImm64(reinterpret_cast<uint64_t>(text))
                              .MovRaxImm64(guest_strlen)
                              .CallRax()
                              .Ret()
                              .Install();
  Check(CallGuest(caller, {}) == sizeof(text) - 1, "T8 imported libC strlen through trap");
  std::puts("PASS T8 imported HLE function through trap");
}

// Guest -> native HLE (through the import trap) -> guest callback via CallGuestFunction.
void TestImportedCallbackRoundTrip() {
  const uint64_t square_plus = X86Code()
                                   .Op({0x48, 0x89, 0xF8})       // mov rax, rdi
                                   .Op({0x48, 0x0F, 0xAF, 0xC0}) // imul rax, rax
                                   .Op({0x48, 0x83, 0xC0, 0x07}) // add rax, 7
                                   .Ret()
                                   .Install();
  const uint64_t doubler = Loader::GuestHost::AddressOf<DoubleViaGuestCallback>("test.DoubleViaGuestCallback");
  const uint64_t caller = X86Code()
                              .MovRdiImm64(square_plus)
                              .MovEsiImm(9)
                              .MovRaxImm64(doubler)
                              .CallRax()
                              .Ret()
                              .Install();
  Check(CallGuest(caller, {}) == 2 * (81 + 7), "T9 guest->HLE->guest callback round trip");
  std::puts("PASS T9 guest->HLE->guest callback round trip");
}

// T10: guest address folding (kernel/addressFold.h). With the fold mask on, guest accesses to
// L | 2^39 reach host memory at L: plain, register-indexed, atomic, REP STOS/MOVS accesses, while
// RDI after REP STOS keeps its unfolded value.
void TestAddressFold() {
  constexpr uint64_t kFoldBit = uint64_t {1} << 39;
  auto* host = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(host != MAP_FAILED, "fold page mmap");
  const auto low = reinterpret_cast<uint64_t>(host);
  Check(low < kFoldBit, "host page below 2^39");

  const uint64_t fn = X86Code()
                          .Op({0x48, 0xC7, 0x07}).Imm32(0x11111111)       // mov qword [rdi], 0x11111111
                          .Op({0xBE}).Imm32(2)                            // mov esi, 2
                          .Op({0xB8}).Imm32(0x22222222)                   // mov eax, 0x22222222
                          .Op({0x48, 0x89, 0x04, 0xF7})                   // mov [rdi + rsi*8], rax
                          .Op({0x48, 0x8B, 0x57, 0x10})                   // mov rdx, [rdi + 16]
                          .Op({0x48, 0x89, 0x57, 0x08})                   // mov [rdi + 8], rdx
                          .Op({0xF0, 0x48, 0x83, 0x47, 0x18, 0x05})       // lock add qword [rdi + 24], 5
                          .Op({0x49, 0x89, 0xF8})                         // mov r8, rdi
                          .Op({0x48, 0x8D, 0x7F, 0x40})                   // lea rdi, [rdi + 64]
                          .Op({0xB9}).Imm32(32)                           // mov ecx, 32
                          .Op({0xB8}).Imm32(0xAB)                         // mov eax, 0xAB
                          .Op({0xF3, 0xAA})                               // rep stosb
                          .Op({0x49, 0x89, 0xF9})                         // mov r9, rdi
                          .Op({0x49, 0x8D, 0x70, 0x40})                   // lea rsi, [r8 + 64]
                          .Op({0x49, 0x8D, 0xB8}).Imm32(0x80)             // lea rdi, [r8 + 128]
                          .Op({0xB9}).Imm32(32)                           // mov ecx, 32
                          .Op({0xF3, 0xA4})                               // rep movsb
                          .Op({0x4C, 0x89, 0xC8})                         // mov rax, r9
                          .Ret()
                          .Install();

  EnableAddressFold(kFoldBit - 1);
  const uint64_t args[1] = {low | kFoldBit};
  const uint64_t rdi_after = CallGuest(fn, args);
  EnableAddressFold(0);

  uint64_t words[4] {};
  std::memcpy(words, host, sizeof(words));
  bool ok = words[0] == 0x11111111 && words[1] == 0x22222222 && words[2] == 0x22222222 && words[3] == 5;
  for (int i = 0; i < 32; i++) {
    ok = ok && host[64 + i] == 0xAB && host[128 + i] == 0xAB;
  }
  Check(ok, "T10 folded guest accesses reach the host page");
  Check(rdi_after == ((low | kFoldBit) + 96), "T10 RDI after REP STOS stays unfolded");
  munmap(host, 4096);
  std::puts("PASS T10 guest address folding");
}

int main() {
  Check(Init(), "translator init");
  Check(AttachCurrentThread(MapStack()), "attach main thread");

  RegisterHle(kAdd5, Add5Thunk);
  RegisterHle(kCallback, CallbackThunk);
  RegisterHle(kSetFs, SetFsThunk);

  TestHleIntegerArgs(MakeHleStub(kAdd5));
  TestHostToGuestCallback(MakeHleStub(kCallback));
  TestFsBase(MakeHleStub(kSetFs));
  TestHostFaultHandler();
  TestHostCreatedGuestThread();
  Check(AttachCurrentThread(MapStack()), "re-attach main thread");
  TestImportedHleFunction();
  TestImportedCallbackRoundTrip();
  TestAddressFold();

  DetachCurrentThread();
  std::puts("GuestCpuFexTests: all cases passed");
  return 0;
}
