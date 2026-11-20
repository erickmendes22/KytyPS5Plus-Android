#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_PROCESSWATCHDOG_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_PROCESSWATCHDOG_H_

#include <array>
#include <atomic>
#include <cstdint>

// GPU-thread watchdog (research/NEXT.md "Stability bug: GPU thread spins inside one submission").
// The GPU thread publishes where it is in the command stream (relaxed atomics, a few stores per
// packet); a background thread checks twice a second and, while one GuestGpu::Process() has run
// longer than kStuckMs, logs "[gpu-watchdog]" lines (kyty log and _perf_log) every kRepeatMs:
// submission, IB address/offset/size/depth, packets and opcode histogram since the submission
// started, the last draw/dispatch, the GPU thread's phase, and the record-thread / walk-pool
// queues. Always on; nothing happens while submissions complete in time.
namespace Libs::Graphics::ProcessWatchdog {

enum class Phase : uint32_t {
	Idle,         // waiting for work
	Process,      // inside GuestGpu::Process (parsing / executing PM4)
	Handler,      // inside a PM4 handler (see last opcode)
	Drain,        // draining the record thread
	SyncCommand,  // running a guest thread's synchronous command
};

struct State {
	std::atomic<int64_t>  start_ns {0};   // Process() start (0: not in Process)
	std::atomic<uint64_t> submission {0}; // submissions processed so far
	std::atomic<uint32_t> phase {0};
	std::atomic<uint64_t> ib_address {0};
	std::atomic<uint32_t> ib_offset_dw {0};
	std::atomic<uint32_t> ib_size_dw {0};
	std::atomic<uint32_t> ib_depth {0};
	std::atomic<uint32_t> last_opcode {0}; // opcode | R << 8 for NOP
	std::atomic<uint64_t> packets {0};     // since start_ns
	std::atomic<uint64_t> ib_switches {0}; // chains/calls followed since start_ns
	std::atomic<uint64_t> draws {0};       // draws/dispatches seen since start_ns
	std::atomic<uint32_t> last_draw_kind {0}; // 1 indexed, 2 auto, 3 dispatch, 4 indirect
	std::atomic<uint32_t> last_draw_count {0};
	std::atomic<uint32_t> last_draw_instances {0};
	std::atomic<uint64_t> last_draw_shader {0}; // VS/CS data address
	std::array<std::atomic<uint32_t>, 256> ops {};
};

State& Get();

// The PM4 thread used to perform two atomic read/modify/writes per packet
// solely to count watchdog packets and opcode histograms. These counters are
// diagnostic (never used for execution decisions); publish them in bounded
// batches instead. The watchdog's exact PC, IB and last opcode continue to
// update on EVERY packet, including the packet on which a shader hangs.
// A watchdog report may lag by at most 1023 packets in its histogram.
struct PacketCounterBatch {
    static constexpr uint32_t FlushEvery = 1024;
    uint32_t queued = 0;
    uint32_t used = 0;
    std::array<uint32_t, 256> count {};
    std::array<uint8_t, 256> indices {};

    void Reset() noexcept {
        for (uint32_t i = 0; i < used; ++i) count[indices[i]] = 0;
        queued = used = 0;
    }

    void Flush(State& state) noexcept {
        if (queued == 0) return;
        state.packets.fetch_add(queued, std::memory_order_relaxed);
        for (uint32_t i = 0; i < used; ++i) {
            const auto index = indices[i];
            state.ops[index].fetch_add(count[index], std::memory_order_relaxed);
        }
        Reset();
    }

    void Note(State& state, uint32_t key) noexcept {
        const auto opcode = key & 255u;
        if (count[opcode]++ == 0u) indices[used++] = static_cast<uint8_t>(opcode);
        if (++queued == FlushEvery) Flush(state);
    }
};

inline PacketCounterBatch& PacketCounters() noexcept {
    static thread_local PacketCounterBatch batch;
    return batch;
}

// GPU thread.
void BeginProcess();
void EndProcess();
inline void SetPhase(Phase phase) {
	Get().phase.store(static_cast<uint32_t>(phase), std::memory_order_relaxed);
}
inline void OnPacket(const uint32_t* ib, uint32_t offset_dw, uint32_t size_dw, uint32_t depth,
                     uint32_t opcode_key) {
	auto& s = Get();
	s.ib_address.store(reinterpret_cast<uint64_t>(ib), std::memory_order_relaxed);
	s.ib_offset_dw.store(offset_dw, std::memory_order_relaxed);
	s.ib_size_dw.store(size_dw, std::memory_order_relaxed);
	s.ib_depth.store(depth, std::memory_order_relaxed);
	s.last_opcode.store(opcode_key, std::memory_order_relaxed);
	PacketCounters().Note(s, opcode_key);
}
inline void OnIbSwitch() {
	Get().ib_switches.fetch_add(1, std::memory_order_relaxed);
}
inline void OnDraw(uint32_t kind, uint32_t count, uint32_t instances, uint64_t shader) {
	auto& s = Get();
	s.draws.fetch_add(1, std::memory_order_relaxed);
	s.last_draw_kind.store(kind, std::memory_order_relaxed);
	s.last_draw_count.store(count, std::memory_order_relaxed);
	s.last_draw_instances.store(instances, std::memory_order_relaxed);
	s.last_draw_shader.store(shader, std::memory_order_relaxed);
}

// Starts the checking thread (once).
void Start();

} // namespace Libs::Graphics::ProcessWatchdog

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_PROCESSWATCHDOG_H_
