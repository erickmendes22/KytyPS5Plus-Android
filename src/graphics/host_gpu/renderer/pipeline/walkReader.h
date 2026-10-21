#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_WALKREADER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_WALKREADER_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Guest reads of an SRT walk that runs on a prepare worker instead of the GPU thread (the fan-out
// pipeline, research/gputhread_pipeline.md section 7).
//
// A worker never calls into the BufferCache, never waits for the GPU and never touches a guest
// page that may be protected: every byte comes from the direct-memory backing alias
// (LibKernel::Memory::TryReadBacking, a second MAP_SHARED view of the memfd that protection never
// changes), so a read cannot fault. The alias holds CPU-visible bytes only, so a read of bytes the
// GPU modified (BufferCache's GPU-modified ranges, as published in a WalkReadView) is refused;
// so is a read the alias cannot serve (no backing, e.g. flexible memory). A refused read makes the
// walk invalid and the recorder walks again on its own thread.
//
// Every range a worker read is recorded (WalkReads). The recorder notes every GPU write it adds
// after a view was published in a WalkWriteJournal; at pickup the walk is still valid when it
// refused nothing and no range noted since its view intersects a range it read.
namespace Libs::Graphics::WalkReader {

// [begin, end) in host-folded addresses (kernel/addressFold.h).
struct Range {
	uint64_t begin = 0;
	uint64_t end   = 0;
};

// Whether any of the sorted, disjoint `ranges` intersects [begin, end).
[[nodiscard]] bool SortedIntersects(std::span<const Range> ranges, uint64_t begin, uint64_t end);

// The GPU-modified bytes as of one point of the recorder's command stream. Immutable once
// published; workers share it through a shared_ptr<const>.
struct View {
	uint64_t           generation = 0; // WalkWriteJournal::Generation() at publish
	std::vector<Range> gpu_modified;   // sorted, disjoint

	// False when [address, address + size) has a GPU-modified byte in this view.
	[[nodiscard]] bool Clean(uint64_t address, uint64_t size) const;
};

// Everything one walk read from guest memory, in host-folded addresses.
struct Reads {
	enum class Refusal : uint8_t { None, GpuModified, NoBacking, Invalid };

	std::vector<Range> ranges; // in read order; contiguous reads merged
	Refusal            first_refusal = Refusal::None;
	uint32_t           refused       = 0;
	uint32_t           reads         = 0;

	void Note(uint64_t begin, uint64_t end);
	void Refuse(Refusal reason);
	[[nodiscard]] bool Refused() const { return refused != 0; }
	void Clear();
};

// GPU writes the recorder added, each with the generation it moved the journal to. Thread-safe:
// the recorder notes and trims, the front publishes views, workers never touch it.
class WriteJournal {
public:
	// At most this many entries are kept; older ones are dropped and any walk whose view is older
	// than the dropped ones counts as invalid.
	static constexpr size_t MaxEntries = 1u << 16u;

	void Note(uint64_t begin, uint64_t end);
	[[nodiscard]] uint64_t Generation() const;
	// True when a range noted after `generation` intersects one of `reads`, or when entries after
	// `generation` were dropped.
	[[nodiscard]] bool WrittenSince(uint64_t generation, std::span<const Range> reads) const;
	// Drops the entries of generations up to `generation` (no live view is that old).
	void Trim(uint64_t generation);
	[[nodiscard]] size_t Size() const;

private:
	struct Entry {
		uint64_t generation = 0;
		Range    range;
	};
	mutable std::mutex m_mutex;
	uint64_t           m_generation = 0;
	uint64_t           m_dropped_up_to = 0; // entries of generations <= this are gone
	std::deque<Entry>  m_entries;
};

// How a worker reaches guest memory. Production: walkReaderGuest.cpp; tests use their own.
struct Memory {
	// Guest -> host address (kernel/addressFold.h); false when the span has no host image.
	bool (*fold)(uint64_t address, uint64_t size, uint64_t* host) = nullptr;
	// Copy from the backing alias; false when it cannot (no backing for these bytes).
	bool (*read_backing)(uint64_t host_address, void* data, uint64_t size) = nullptr;
};
[[nodiscard]] Memory GuestMemory();

// One walk on a worker: the SRT runtime readers and the thread reader point here.
class WorkerWalk {
public:
	WorkerWalk(std::shared_ptr<const View> view, Memory memory);
	WorkerWalk(const WorkerWalk&)            = delete;
	WorkerWalk& operator=(const WorkerWalk&) = delete;

	// `base` with its specialization, branch and (when set) vector readers routed through this
	// walk; with no vector reader, ordinary reads use ThreadReader(). `mapped` stays the base's.
	[[nodiscard]] ShaderRecompiler::IR::SrtRuntime Runtime(const ShaderRecompiler::IR::SrtRuntime& base);

	// Routes this thread's SRT host reads here while alive (ScopedSrtThreadReader).
	[[nodiscard]] ShaderRecompiler::IR::ScopedSrtThreadReader ThreadReader();

	[[nodiscard]] const Reads& GetReads() const { return m_reads; }
	[[nodiscard]] Reads        TakeReads() { return std::move(m_reads); }
	[[nodiscard]] uint64_t     Generation() const { return m_view->generation; }

	// The worker read: fold, record, refuse GPU-modified or unbacked bytes, copy from the alias.
	bool Read(uint64_t address, void* data, size_t size);

private:
	static bool ReadValues(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadThread(void* userdata, uint64_t address, void* data, size_t size);

	std::shared_ptr<const View> m_view;
	Memory                      m_memory;
	Reads                       m_reads;
};

// Recorder-side pickup check of a worker walk: valid when it refused nothing and no GPU write
// noted after its view intersects what it read.
[[nodiscard]] bool StillValid(const Reads& reads, uint64_t view_generation, const WriteJournal& journal);

// Publishes views: the recorder (or the front, between items) calls Publish with the current
// GPU-modified ranges whenever the journal moved; workers take Current(). Thread-safe.
class ViewPublisher {
public:
	// `for_each` visits the current GPU-modified ranges in address order: void(uint64_t begin, uint64_t end).
	template <typename ForEach>
	void Publish(uint64_t generation, ForEach&& for_each) {
		if (auto current = Current(); current != nullptr && current->generation == generation) {
			return;
		}
		auto view        = std::make_shared<View>();
		view->generation = generation;
		for_each([&](uint64_t begin, uint64_t end) { view->gpu_modified.push_back({begin, end}); });
		Store(std::move(view));
	}
	[[nodiscard]] std::shared_ptr<const View> Current() const;

private:
	void Store(std::shared_ptr<const View> view);

	mutable std::mutex          m_mutex;
	std::shared_ptr<const View> m_view;
};

} // namespace Libs::Graphics::WalkReader

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_WALKREADER_H_
