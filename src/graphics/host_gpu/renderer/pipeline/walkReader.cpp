#include "graphics/host_gpu/renderer/pipeline/walkReader.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics::WalkReader {

bool SortedIntersects(std::span<const Range> ranges, uint64_t begin, uint64_t end) {
	if (begin >= end) {
		return false;
	}
	// First range ending after `begin`.
	const auto it = std::upper_bound(ranges.begin(), ranges.end(), begin,
	                                 [](uint64_t address, const Range& range) { return address < range.end; });
	return it != ranges.end() && it->begin < end;
}

bool View::Clean(uint64_t address, uint64_t size) const {
	return size == 0 || !SortedIntersects(gpu_modified, address, address + size);
}

void Reads::Note(uint64_t begin, uint64_t end) {
	++reads;
	if (!ranges.empty() && ranges.back().end == begin) {
		ranges.back().end = end;
		return;
	}
	ranges.push_back({begin, end});
}

void Reads::Refuse(Refusal reason) {
	if (refused++ == 0) {
		first_refusal = reason;
	}
}

void Reads::Clear() {
	ranges.clear();
	first_refusal = Refusal::None;
	refused       = 0;
	reads         = 0;
}

void WriteJournal::Note(uint64_t begin, uint64_t end) {
	if (begin >= end) {
		return;
	}
	std::lock_guard lock(m_mutex);
	++m_generation;
	if (m_entries.size() >= MaxEntries) {
		m_dropped_up_to = m_entries.front().generation;
		m_entries.pop_front();
	}
	m_entries.push_back({m_generation, {begin, end}});
}

uint64_t WriteJournal::Generation() const {
	std::lock_guard lock(m_mutex);
	return m_generation;
}

bool WriteJournal::WrittenSince(uint64_t generation, std::span<const Range> reads) const {
	std::lock_guard lock(m_mutex);
	if (generation >= m_generation) {
		return false;
	}
	if (generation < m_dropped_up_to) {
		return true; // writes after this view were dropped: cannot prove the reads unchanged
	}
	// Entries are in generation order; the first one after `generation`.
	auto it = std::upper_bound(m_entries.begin(), m_entries.end(), generation,
	                           [](uint64_t g, const Entry& entry) { return g < entry.generation; });
	for (; it != m_entries.end(); ++it) {
		for (const auto& read: reads) {
			if (read.begin < it->range.end && it->range.begin < read.end) {
				return true;
			}
		}
	}
	return false;
}

void WriteJournal::Trim(uint64_t generation) {
	std::lock_guard lock(m_mutex);
	while (!m_entries.empty() && m_entries.front().generation <= generation) {
		m_entries.pop_front();
	}
	// A view older than this can no longer be checked: WrittenSince reports it as written.
	m_dropped_up_to = std::max(m_dropped_up_to, generation);
}

size_t WriteJournal::Size() const {
	std::lock_guard lock(m_mutex);
	return m_entries.size();
}

WorkerWalk::WorkerWalk(std::shared_ptr<const View> view, Memory memory): m_view(std::move(view)), m_memory(memory) {
	if (m_view == nullptr) {
		m_view = std::make_shared<const View>();
	}
}

bool WorkerWalk::Read(uint64_t address, void* data, size_t size) {
	uint64_t host = address;
	if (size == 0) {
		return true;
	}
	if (data == nullptr || UINT64_MAX - address < size || m_memory.fold == nullptr ||
	    m_memory.read_backing == nullptr || !m_memory.fold(address, size, &host)) {
		m_reads.Refuse(Reads::Refusal::Invalid);
		return false;
	}
	m_reads.Note(host, host + size);
	if (!m_view->Clean(host, size)) {
		m_reads.Refuse(Reads::Refusal::GpuModified);
		return false;
	}
	if (!m_memory.read_backing(host, data, size)) {
		m_reads.Refuse(Reads::Refusal::NoBacking);
		return false;
	}
	return true;
}

bool WorkerWalk::ReadValues(void* userdata, uint64_t address, std::span<uint32_t> values) {
	return !values.empty() && static_cast<WorkerWalk*>(userdata)->Read(address, values.data(), values.size_bytes());
}

bool WorkerWalk::ReadThread(void* userdata, uint64_t address, void* data, size_t size) {
	return static_cast<WorkerWalk*>(userdata)->Read(address, data, size);
}

ShaderRecompiler::IR::SrtRuntime WorkerWalk::Runtime(const ShaderRecompiler::IR::SrtRuntime& base) {
	auto runtime = base;
	if (runtime.read_memory != nullptr) {
		// The shared userdata now names this walk, so a custom vector reader reads through it too.
		runtime.read_memory = ReadValues;
	}
	runtime.userdata                   = this;
	runtime.read_specialization_memory = ReadValues;
	runtime.read_branch_memory         = ReadValues;
	return runtime;
}

ShaderRecompiler::IR::ScopedSrtThreadReader WorkerWalk::ThreadReader() {
	return ShaderRecompiler::IR::ScopedSrtThreadReader(ReadThread, this);
}

bool StillValid(const Reads& reads, uint64_t view_generation, const WriteJournal& journal) {
	return !reads.Refused() && !journal.WrittenSince(view_generation, reads.ranges);
}

std::shared_ptr<const View> ViewPublisher::Current() const {
	std::lock_guard lock(m_mutex);
	return m_view;
}

void ViewPublisher::Store(std::shared_ptr<const View> view) {
	std::lock_guard lock(m_mutex);
	// Two publishers (front and recorder) may race: never replace a view with an older one.
	if (m_view != nullptr && m_view->generation > view->generation) {
		return;
	}
	m_view = std::move(view);
}

} // namespace Libs::Graphics::WalkReader
