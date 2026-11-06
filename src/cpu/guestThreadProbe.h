#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace Cpu::ThreadProbe {
struct Snapshot {
    uint64_t rip = 0, host_pc = 0, host_lr = 0, module_base = 0;
    std::array<uint64_t, 16> gpr{};
    bool exact = false;
};
struct Mailbox {
    std::atomic<uint64_t> sequence{0};
    std::array<std::atomic<uint64_t>, 21> values{};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
inline std::array<Mailbox, 256> mailboxes;
using Request = void (*)(uint64_t tid);
inline std::atomic<Request> request{nullptr};
// Called on the interrupted thread: lock-free stores only; no logging/allocation.
inline void Publish(size_t slot, const Snapshot& data) {
    if (slot >= mailboxes.size()) return;
    auto& box = mailboxes[slot];
    box.sequence.fetch_add(1, std::memory_order_seq_cst);
    const std::array<uint64_t, 5> head{data.rip, data.host_pc, data.host_lr, data.module_base, data.exact};
    for (size_t i = 0; i < head.size(); ++i) box.values[i].store(head[i], std::memory_order_seq_cst);
    for (size_t i = 0; i < data.gpr.size(); ++i) box.values[i+5].store(data.gpr[i], std::memory_order_seq_cst);
    box.sequence.fetch_add(1, std::memory_order_seq_cst);
}
inline bool Read(size_t slot, Snapshot& data) {
    if (slot >= mailboxes.size()) return false;
    const auto& box = mailboxes[slot];
    const auto start = box.sequence.load(std::memory_order_seq_cst);
    if (!start || start % 2) return false;
    data.rip = box.values[0].load(std::memory_order_seq_cst);
    data.host_pc = box.values[1].load(std::memory_order_seq_cst);
    data.host_lr = box.values[2].load(std::memory_order_seq_cst);
    data.module_base = box.values[3].load(std::memory_order_seq_cst);
    data.exact = box.values[4].load(std::memory_order_seq_cst) != 0;
    for (size_t i = 0; i < data.gpr.size(); ++i) data.gpr[i] = box.values[i+5].load(std::memory_order_seq_cst);
    return box.sequence.load(std::memory_order_seq_cst) == start;
}
inline void Dump(size_t slot, uint64_t tid) {
    Snapshot data;
    if (!Read(slot, data)) return;
    std::printf("[DS_SAMPLE] tid=%llu guest_rip=%016llx exact_jit=%d host_pc=%016llx host_lr=%016llx module_base=%016llx\n",
        static_cast<unsigned long long>(tid), static_cast<unsigned long long>(data.rip), data.exact,
        static_cast<unsigned long long>(data.host_pc), static_cast<unsigned long long>(data.host_lr),
        static_cast<unsigned long long>(data.module_base));
    for (size_t i = 0; i < data.gpr.size(); i += 4) {
        std::printf("[DS_SAMPLE] tid=%llu regs[%zu..%zu]=%016llx/%016llx/%016llx/%016llx\n",
            static_cast<unsigned long long>(tid), i, i+3,
            static_cast<unsigned long long>(data.gpr[i]), static_cast<unsigned long long>(data.gpr[i+1]),
            static_cast<unsigned long long>(data.gpr[i+2]), static_cast<unsigned long long>(data.gpr[i+3]));
    }
}
} // namespace Cpu::ThreadProbe
