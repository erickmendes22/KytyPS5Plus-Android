#include "graphics/guest_gpu/processWatchdog.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics::ProcessWatchdog {
State& Get() {
    static State state;
    return state;
}
}

int main() {
    using namespace Libs::Graphics::ProcessWatchdog;
    auto& s = Get();
    auto& batch = PacketCounters();
    batch.Reset();
    for (auto& v : s.ops) v.store(0, std::memory_order_relaxed);
    s.packets.store(0, std::memory_order_relaxed);

    constexpr uint32_t kA = 0x2cu, kB = 0x7au;
    constexpr uint32_t kFirst = PacketCounterBatch::FlushEvery;
    // Check that the histogram records every packet with no loss, including
    // the 1024th one where the batch is atomically published.
    for (uint32_t i = 0; i < kFirst; ++i) {
        const uint32_t key = (i % 4 == 0) ? kB : kA;
        OnPacket(reinterpret_cast<const uint32_t*>(uintptr_t{0x40000}),
                 i, 2048, 1, key);
        assert(s.last_opcode.load(std::memory_order_relaxed) == key);
        assert(s.ib_offset_dw.load(std::memory_order_relaxed) == i);
    }
    assert(s.packets.load(std::memory_order_relaxed) == kFirst);
    assert(s.ops[kA].load(std::memory_order_relaxed) == 768);
    assert(s.ops[kB].load(std::memory_order_relaxed) == 256);

    // The watchdog's current address/opcode stays exact even while
    // cumulative histogram counters await the next safe flush.
    for (uint32_t i = 0; i < 17; ++i)
        OnPacket(reinterpret_cast<const uint32_t*>(uintptr_t{0x58000}),
                 i, 48, 3, kB);
    assert(s.ib_address.load(std::memory_order_relaxed) == 0x58000);
    assert(s.ib_depth.load(std::memory_order_relaxed) == 3);
    assert(s.last_opcode.load(std::memory_order_relaxed) == kB);
    assert(s.ops[kB].load(std::memory_order_relaxed) == 256);
    batch.Flush(s);
    assert(s.packets.load(std::memory_order_relaxed) == kFirst + 17);
    assert(s.ops[kB].load(std::memory_order_relaxed) == 273);
    assert(s.ops[kA].load(std::memory_order_relaxed) == 768);
    assert(batch.queued == 0 && batch.used == 0);

    // Clearing a discarded submission must not contaminate its successor.
    OnPacket(reinterpret_cast<const uint32_t*>(uintptr_t{0x70000}), 7, 9, 2, kA);
    batch.Reset();
    OnPacket(reinterpret_cast<const uint32_t*>(uintptr_t{0x80000}), 0, 8, 1, kB);
    batch.Flush(s);
    assert(s.packets.load(std::memory_order_relaxed) == kFirst + 18);
    assert(s.ops[kA].load(std::memory_order_relaxed) == 768);
    assert(s.ops[kB].load(std::memory_order_relaxed) == 274);
    std::puts("PM4 watchdog batching: exact current packet, bounded histogram and reset OK");
}
