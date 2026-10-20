#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEPROBE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEPROBE_H_

#include <cstddef>
#include <cstdint>

// Sizing probe for a two-stage GPU thread (research/gputhread_pipeline.md), --perf-stats only.
//
// The GPU thread today prepares a draw or dispatch (PM4 state, SRT walk, materialize, program
// lookup: the future front stage) and then binds and records it (the back stage). The probe
// simulates a front stage running up to kWindow items ahead of the back stage and counts the
// points where it would have to stop and wait for the back stage to catch up ("drains"):
//   - a walker read of guest memory that an item still in the window writes on the GPU,
//   - a walker read while an item in the window may write through raw device addresses (BDA),
//   - a walker read of GPU-dirty memory (in-place readback with a semaphore wait),
//   - a synchronous command from a guest thread (fault readbacks, SendCommandSync),
//   - indirect draw arguments read on the CPU.
// It also counts accesses to the live register blocks during the record phase (what a register
// snapshot must cover) and the bytes of materialized resources per item (the per-item copy C2's
// note requires). Everything is counted on the GPU thread; nothing changes behaviour.
namespace Libs::Graphics::PipelineProbe {

inline constexpr uint64_t kWindow = 64;

enum class Drain : uint8_t { SrtWrite, SrtDma, SrtDirty, SyncCommand, IndirectCpu, Count };

// Pure model of the window and the drain decisions; the GPU-thread hooks below drive one
// instance. Exposed for tests.
class Model {
public:
	void BeginItem();
	void MarkDmaWriter();
	void OnGpuWrite(uint64_t address, uint64_t size);
	// Returns the drain this read causes, or Drain::Count when it needs none.
	Drain OnSrtRead(uint64_t address, uint64_t size, bool gpu_dirty);
	void  OnDrain();
	[[nodiscard]] uint64_t Item() const { return m_item; }

private:
	struct Write {
		uint64_t begin = 0;
		uint64_t end   = 0;
		uint64_t item  = 0;
	};
	static constexpr size_t kRing = 1024;
	[[nodiscard]] uint64_t Oldest() const;

	Write    m_writes[kRing] {};
	size_t   m_write_count  = 0;
	size_t   m_write_next   = 0;
	uint64_t m_item         = 0;
	uint64_t m_drained      = 0; // items <= this one are recorded and submitted
	uint64_t m_last_dma     = 0; // last item that may write through raw device addresses
};

inline uint64_t Model::Oldest() const {
	// Items after the last drain and inside the window are not recorded yet.
	const uint64_t window_start = m_item > kWindow ? m_item - kWindow : 0;
	return (m_drained > window_start ? m_drained : window_start) + 1;
}

inline void Model::BeginItem() {
	m_item++;
}

inline void Model::MarkDmaWriter() {
	m_last_dma = m_item;
}

inline void Model::OnGpuWrite(uint64_t address, uint64_t size) {
	if (size == 0) {
		return;
	}
	m_writes[m_write_next] = {address, address + size, m_item};
	m_write_next           = (m_write_next + 1) % kRing;
	if (m_write_count < kRing) {
		m_write_count++;
	}
}

inline Drain Model::OnSrtRead(uint64_t address, uint64_t size, bool gpu_dirty) {
	if (size == 0) {
		return Drain::Count;
	}
	const uint64_t oldest = Oldest();
	auto           cause  = Drain::Count;
	// The item being prepared cannot depend on its own writes (they are recorded after it).
	if (m_last_dma >= oldest && m_last_dma < m_item) {
		cause = Drain::SrtDma;
	} else {
		const uint64_t end = address + size;
		for (size_t i = 0; i < m_write_count; i++) {
			const auto& w = m_writes[(m_write_next + kRing - 1 - i) % kRing];
			if (w.item < oldest) {
				break; // older entries are all recorded
			}
			if (w.item < m_item && w.begin < end && address < w.end) {
				cause = Drain::SrtWrite;
				break;
			}
		}
		if (cause == Drain::Count && gpu_dirty) {
			cause = Drain::SrtDirty;
		}
	}
	if (cause != Drain::Count) {
		OnDrain();
	}
	return cause;
}

inline void Model::OnDrain() {
	// The back stage has recorded and submitted everything before the current item.
	m_drained = m_item > 0 ? m_item - 1 : 0;
}

// GPU-thread hooks (no-ops unless --perf-stats is on).
void BeginItem();
void MarkDmaWriter();
void AddResourceBytes(size_t bytes);
void OnGpuWrite(uint64_t address, uint64_t size);
void OnSrtRead(uint64_t address, uint64_t size, bool gpu_dirty);
void OnDrain(Drain cause);
void OnWaitRegMemBlocked();

// Live register block accesses (CommandBuffer::GetRegisters/GetUserConfig/GetShaders) while a
// record phase is open on this thread.
inline thread_local int t_record_depth = 0;
class ScopedRecordPhase {
public:
	ScopedRecordPhase() { t_record_depth++; }
	~ScopedRecordPhase() { t_record_depth--; }
	ScopedRecordPhase(const ScopedRecordPhase&)            = delete;
	ScopedRecordPhase& operator=(const ScopedRecordPhase&) = delete;
};
void CountRegisterBlockAccess();
inline void OnRegisterBlockAccess() {
	if (t_record_depth > 0) {
		CountRegisterBlockAccess();
	}
}

// Bytes a per-item copy of a stage's materialized resources would take.
template <typename Snapshot>
[[nodiscard]] size_t ResourceBytes(const Snapshot* r) {
	if (r == nullptr) return 0;
	return r->buffers.size() * sizeof(r->buffers[0]) + r->images.size() * sizeof(r->images[0]) +
	       r->samplers.size() * sizeof(r->samplers[0]) + r->flattened_srt.size() * 4u +
	       r->user_data.size() * 4u + r->specialization_reads.size() * 16u;
}

} // namespace Libs::Graphics::PipelineProbe

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEPROBE_H_
