#pragma once
#include "cpu/guestThreadProbe.h"
// Per-thread HLE state survives ring wrap. Enabled only by the existing DS trace.
// Observations never release a guest wait or alter command ordering.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <atomic>
#include <pthread.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace Cpu::HleDiagnostics {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("KYTY_DS_FRAME_TRACE");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}
struct State {
    uint32_t id = 0;
    uint64_t caller = 0;
    std::array<uint64_t, 3> args{};
    std::string_view symbol;
    std::chrono::steady_clock::time_point since{};
    bool active = false;
};
struct Slot {
    std::mutex mutex;
    State state;
    uint64_t tid = 0;
    char name[32]{};
};
inline std::array<Slot, 256> slots;
inline std::atomic<size_t> next_slot{0};
inline thread_local Slot* registered_slot = nullptr;
inline Slot* CurrentSlot() {
    static thread_local Slot* slot = []() -> Slot* {
        const auto index = next_slot.fetch_add(1, std::memory_order_relaxed);
        if (index >= slots.size()) return nullptr;
        auto& entry = slots[index];
        std::lock_guard lock(entry.mutex);
#if defined(__linux__)
        entry.tid = static_cast<uint64_t>(::syscall(SYS_gettid));
#else
        entry.tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
        pthread_getname_np(pthread_self(), entry.name, sizeof(entry.name));
        return &entry;
    }();
    registered_slot = slot;
    return slot;
}
class Scope {
    Slot* slot = nullptr;
    State previous;
public:
    Scope(uint32_t id, std::string_view symbol, uint64_t caller,
          uint64_t a0, uint64_t a1, uint64_t a2) {
        if (!Enabled() || !(slot = CurrentSlot())) return;
        std::lock_guard lock(slot->mutex);
        previous = slot->state;
        slot->state = {id, caller, {a0, a1, a2}, symbol,
                       std::chrono::steady_clock::now(), true};
    }
    ~Scope() {
        if (!slot) return;
        std::lock_guard lock(slot->mutex);
        if (previous.active) slot->state = previous; // nested CallGuest/HLE
        else {
            slot->state.active = false;
            slot->state.since = std::chrono::steady_clock::now();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline void Dump() {
    if (!Enabled()) return;
    for (auto& entry : slots) {
        std::unique_lock lock(entry.mutex, std::try_to_lock);
        if (!lock || !entry.tid) continue;
        const auto& state = entry.state;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - state.since).count();
        std::printf("[DS_THREAD] tid=%llu name=%s phase=%s age_ms=%lld id=%u symbol=%.*s caller=%016llx args=%016llx/%016llx/%016llx\n",
            static_cast<unsigned long long>(entry.tid), entry.name,
            state.active ? "hle" : "guest_or_exited", static_cast<long long>(age),
            state.id, static_cast<int>(state.symbol.size()), state.symbol.data(),
            static_cast<unsigned long long>(state.caller),
            static_cast<unsigned long long>(state.args[0]),
            static_cast<unsigned long long>(state.args[1]),
            static_cast<unsigned long long>(state.args[2]));
        const auto tid = entry.tid;
        // A worker inside an HLE wait can hold the missing game task. Sampling
        // only guest loops hides precisely that dependency. pthread_exit does
        // not unwind this scope, so its old record must not be sampled.
        const bool stale_thread = age >= 2000 &&
            (!state.active || state.symbol != "LibKernel::PthreadExit");
        const auto index = static_cast<size_t>(&entry - slots.data());
        lock.unlock();
        Cpu::ThreadProbe::Dump(index, tid);
        if (stale_thread) {
            if (auto probe = Cpu::ThreadProbe::request.load(std::memory_order_acquire)) probe(tid);
        }
    }
}
} // namespace Cpu::HleDiagnostics
