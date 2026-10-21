#include "graphics/guest_gpu/command_processor/frontOnlyPackets.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

// KYTY_DRAIN_LITE: the packets it stops draining are exactly the four front-only ones, their
// bodies only read the packet words (the packet page is read-only here: a write faults), and the
// only state they change is the context block and its push/pop copy handed to them.

namespace {

namespace Pm4  = Libs::Graphics::Pm4;
namespace Fop  = Libs::Graphics::FrontOnlyPackets;

static_assert(!Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_POP_MARKER, false));
static_assert(Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_POP_MARKER, true));
static_assert(Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_PUSH_MARKER, true));
static_assert(Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_CONTEXT_STATE, true));
static_assert(Fop::IsPipelined(Pm4::IT_ACQUIRE_MEM, 0, true));
static_assert(!Fop::IsPipelined(Pm4::IT_ACQUIRE_MEM, 0, false));
// Packets that write guest memory, record GPU work or touch the renderer keep draining.
static_assert(!Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_RELEASE_MEM, true));
static_assert(!Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_WRITE_DATA, true));
static_assert(!Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_DMA_DATA, true));
static_assert(!Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_FLIP, true));
static_assert(!Fop::IsPipelined(Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE, true));
static_assert(!Fop::IsPipelined(Pm4::IT_EVENT_WRITE, 0, true));
static_assert(Fop::MarkerBodyDwords(KYTY_PM4(5, Pm4::IT_NOP, Pm4::R_PUSH_MARKER)) == 4);
static_assert(Fop::AcquireMemValid(0xc0055800u) && Fop::AcquireMemValid(0xc0065800u));
static_assert(!Fop::AcquireMemValid(0xc0075800u));
static_assert(Fop::AcquireMemBodyDwords(0xc0065800u) == 7);

int g_failures = 0;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "FrontOnlyPacketsTests: failed: %s\n", message);
    g_failures++;
  }
}

// A context block with canaries on both sides: a write outside the block shows up.
struct FakeContext {
  uint32_t regs[64];
  void Reset() { std::memset(regs, 0, sizeof(regs)); }
};
struct Guarded {
  uint64_t before = 0x1111222233334444ull;
  FakeContext ctx{};
  FakeContext saved{};
  bool pushed = false;
  uint64_t after = 0x5555666677778888ull;
};

bool CanariesIntact(const Guarded &g) {
  return g.before == 0x1111222233334444ull && g.after == 0x5555666677778888ull;
}

void CheckDrainDelta() {
  // Over every opcode and custom code, drain_lite adds exactly the four front-only packets.
  uint32_t added = 0;
  bool removed = false;
  for (uint32_t op = 0; op < 256; op++) {
    for (uint32_t r = 0; r < Pm4::R_NUM; r++) {
      const bool off = Fop::IsPipelined(op, r, false);
      const bool on = Fop::IsPipelined(op, r, true);
      removed = removed || (off && !on);
      if (on && !off && (op == Pm4::IT_NOP || r == 0)) {
        added++;
      }
    }
  }
  Check(!removed, "drain_lite drains a packet that was pipelined");
  Check(added == 4, "drain_lite adds a packet beyond markers, ACQUIRE_MEM and R_CONTEXT_STATE");
}

void CheckReadOnlyPackets() {
  const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void *mem = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(mem != MAP_FAILED, "mmap");
  if (mem == MAP_FAILED) {
    return;
  }
  auto *words = static_cast<uint32_t *>(mem);
  words[0] = KYTY_PM4(5, Pm4::IT_NOP, Pm4::R_PUSH_MARKER);
  std::memcpy(&words[1], "frame pass\0\0\0\0\0", 16);
  words[5] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_POP_MARKER);
  words[6] = 0xc0065800u; // ACQUIRE_MEM
  words[14] = KYTY_PM4(3, Pm4::IT_NOP, Pm4::R_CONTEXT_STATE);
  words[15] = Fop::kContextPushClear;
  words[16] = KYTY_PM4(3, Pm4::IT_NOP, Pm4::R_CONTEXT_STATE);
  words[17] = Fop::kContextPop;
  uint32_t before[32];
  std::memcpy(before, words, sizeof(before));
  Check(mprotect(mem, page, PROT_READ) == 0, "mprotect");

  const uint32_t *packets = words;
  Check(Fop::MarkerBodyDwords(packets[0]) == 4, "push marker length");
  Check(Fop::MarkerBodyDwords(packets[5]) == 1, "pop marker length");
  Check(Fop::AcquireMemValid(packets[6]) && Fop::AcquireMemBodyDwords(packets[6]) == 7,
        "acquire_mem length");

  Guarded g;
  for (uint32_t i = 0; i < 64; i++) {
    g.ctx.regs[i] = i + 1;
  }
  const FakeContext original = g.ctx;
  Check(Fop::ApplyContextState(packets[15], g.ctx, g.saved, g.pushed), "push-clear");
  Check(g.pushed && g.ctx.regs[7] == 0 && g.saved.regs[7] == 8, "push-clear state");
  Check(Fop::ApplyContextState(packets[17], g.ctx, g.saved, g.pushed), "pop");
  Check(!g.pushed && std::memcmp(&g.ctx, &original, sizeof(original)) == 0, "pop restores");
  Check(g.saved.regs[7] == 0, "pop clears the saved copy");
  Check(CanariesIntact(g), "context-state op wrote outside its blocks");
  Check(std::memcmp(before, packets, sizeof(before)) == 0, "packet words changed");
  munmap(mem, page);
}

void CheckContextStateErrors() {
  Guarded g;
  g.ctx.regs[0] = 42;
  Check(Fop::ApplyContextState(Fop::kContextPush, g.ctx, g.saved, g.pushed), "push");
  const Guarded snapshot = g;
  Check(!Fop::ApplyContextState(Fop::kContextPush, g.ctx, g.saved, g.pushed), "double push fails");
  Check(!Fop::ApplyContextState(Fop::kContextPushClear, g.ctx, g.saved, g.pushed),
        "push-clear while pushed fails");
  Check(!Fop::ApplyContextState(7, g.ctx, g.saved, g.pushed), "unknown op fails");
  Check(std::memcmp(&g.ctx, &snapshot.ctx, sizeof(g.ctx)) == 0 &&
            std::memcmp(&g.saved, &snapshot.saved, sizeof(g.saved)) == 0 && g.pushed == snapshot.pushed,
        "a failed op changed state");
  Check(Fop::ApplyContextState(Fop::kContextPop, g.ctx, g.saved, g.pushed), "pop");
  Check(!Fop::ApplyContextState(Fop::kContextPop, g.ctx, g.saved, g.pushed), "pop without push fails");
  Check(Fop::ApplyContextState(Fop::kContextClear, g.ctx, g.saved, g.pushed) && g.ctx.regs[0] == 0,
        "clear");
  Check(CanariesIntact(g), "canaries");
}

} // namespace

int main() {
  CheckDrainDelta();
  CheckReadOnlyPackets();
  CheckContextStateErrors();
  if (g_failures != 0) return 1;
  std::printf("FrontOnlyPacketsTests: ok\n");
  return 0;
}
