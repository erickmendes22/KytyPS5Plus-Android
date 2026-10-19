#include "graphics/shader/recompiler/ir/passes/ResourceMemo.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <atomic>
#include <bit>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

std::atomic<bool> g_enabled {false};

// Counters of the GPU thread's memos (relaxed: read once a second for the perf log).
std::atomic<uint64_t> g_hits {0};
std::atomic<uint64_t> g_misses {0};
std::atomic<uint64_t> g_stale {0};
std::atomic<uint64_t> g_uncached {0};
std::atomic<uint64_t> g_evicted {0};

// KYTY_SRT_MEMO_TRACE: per differing key word (user data 0..63, 64 = shader base, 65 = dispatch
// inputs, 66 = user-data size) how many misses differed there from the closest entry.
constexpr size_t TraceSlots = 67;
std::array<std::atomic<uint64_t>, TraceSlots> g_trace_words {};
std::array<std::atomic<uint64_t>, 5>          g_trace_distance {}; // 1, 2, 3, 4-7, 8+ words
bool TraceEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SRT_MEMO_TRACE");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void Bump(std::atomic<uint64_t>& counter) {
	counter.store(counter.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

// Results kept per shader source (KYTY_SRT_MEMO_SLOTS / live knob memo_slots, default 32).
std::atomic<size_t> g_max_entries {0};

size_t MaxEntries() {
	const auto slots = g_max_entries.load(std::memory_order_relaxed);
	return slots != 0 ? slots : ResourceMemo::SlotsStartValue();
}

uint64_t HashWords(const std::vector<uint32_t>& words) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const auto word: words) {
		hash = (hash ^ word) * 0x100000001b3ull;
		hash ^= hash >> 29u;
	}
	return hash;
}

} // namespace

void ResourceMemo::SetEnabled(bool enabled) {
	g_enabled.store(enabled, std::memory_order_relaxed);
}

bool ResourceMemo::Enabled() {
	return g_enabled.load(std::memory_order_relaxed);
}

size_t ResourceMemo::SlotsStartValue() {
	static const size_t slots = [] {
		const char* value = std::getenv("KYTY_SRT_MEMO_SLOTS");
		return value != nullptr ? static_cast<size_t>(std::strtoul(value, nullptr, 10)) : size_t {32};
	}();
	return std::clamp<size_t>(slots, 1, 1024);
}

void ResourceMemo::SetSlots(size_t slots) {
	g_max_entries.store(std::clamp<size_t>(slots, 1, 1024), std::memory_order_relaxed);
}

bool ResourceMemo::StartValue() {
	const char* value = std::getenv("KYTY_SRT_MEMO");
	return value != nullptr && value[0] == '1';
}

ResourceMemo::Stats ResourceMemo::TakeStats() {
	return {.hits     = g_hits.exchange(0, std::memory_order_relaxed),
	        .misses   = g_misses.exchange(0, std::memory_order_relaxed),
	        .stale    = g_stale.exchange(0, std::memory_order_relaxed),
	        .uncached = g_uncached.exchange(0, std::memory_order_relaxed),
	        .evicted  = g_evicted.exchange(0, std::memory_order_relaxed)};
}

std::string ResourceMemo::TakeMissReport() {
	if (!TraceEnabled()) return {};
	std::array<std::pair<uint64_t, size_t>, TraceSlots> counts {};
	uint64_t total = 0;
	for (size_t i = 0; i < TraceSlots; ++i) {
		counts[i] = {g_trace_words[i].exchange(0, std::memory_order_relaxed), i};
		total += counts[i].first;
	}
	if (total == 0) return {};
	std::ranges::sort(counts, std::greater {});
	std::string report = "diff words:";
	char        part[48];
	for (size_t i = 0; i < 8 && counts[i].first != 0; ++i) {
		const auto slot = counts[i].second;
		if (slot < 64) std::snprintf(part, sizeof(part), " ud%zu=%llu", slot, static_cast<unsigned long long>(counts[i].first));
		else std::snprintf(part, sizeof(part), " %s=%llu", slot == 64 ? "base" : slot == 65 ? "dispatch" : "size",
		                   static_cast<unsigned long long>(counts[i].first));
		report += part;
	}
	report += " | closest differs in";
	const char* names[] = {"1", "2", "3", "4-7", "8+"};
	for (size_t i = 0; i < g_trace_distance.size(); ++i) {
		std::snprintf(part, sizeof(part), " %s:%llu", names[i],
		              static_cast<unsigned long long>(g_trace_distance[i].exchange(0, std::memory_order_relaxed)));
		report += part;
	}
	return report;
}

void ResourceMemo::TraceMiss() const {
	// Key layout (BuildKey): size, the masked user-data words in index order, base (2), dispatch.
	std::vector<size_t> slots;
	slots.push_back(66);
	for (size_t word = 0; word < m_user_data_words.size(); ++word) {
		for (auto bits = m_user_data_words[word]; bits != 0u; bits &= bits - 1u) {
			slots.push_back(std::min<size_t>(word * 64u + static_cast<size_t>(std::countr_zero(bits)), 63u));
		}
	}
	slots.push_back(64);
	slots.push_back(64);
	const Entry* closest = nullptr;
	size_t       best    = SIZE_MAX;
	for (const auto& entry: m_entries) {
		if (entry.key.size() != m_key.size()) continue;
		size_t differ = 0;
		for (size_t i = 0; i < m_key.size(); ++i) differ += entry.key[i] != m_key[i] ? 1u : 0u;
		if (differ < best) {
			best    = differ;
			closest = &entry;
		}
	}
	if (closest == nullptr) return;
	for (size_t i = 0; i < m_key.size(); ++i) {
		if (closest->key[i] == m_key[i]) continue;
		const auto slot = i < slots.size() ? slots[i] : 65u;
		g_trace_words[slot].fetch_add(1, std::memory_order_relaxed);
	}
	const size_t bucket = best <= 3 ? best - 1 : best < 8 ? 3 : 4;
	if (best != 0) g_trace_distance[bucket].fetch_add(1, std::memory_order_relaxed);
}

void ResourceMemo::BuildKey(const SrtRuntime& runtime) {
	m_key.clear();
	m_key.push_back(static_cast<uint32_t>(runtime.user_data.size()));
	for (size_t word = 0; word < m_user_data_words.size(); ++word) {
		for (auto bits = m_user_data_words[word]; bits != 0u; bits &= bits - 1u) {
			const auto index = word * 64u + static_cast<size_t>(std::countr_zero(bits));
			m_key.push_back(index < runtime.user_data.size() ? runtime.user_data[index] : 0u);
		}
	}
	m_key.push_back(static_cast<uint32_t>(runtime.shader_base));
	m_key.push_back(static_cast<uint32_t>(runtime.shader_base >> 32u));
	m_key.push_back(static_cast<uint32_t>(runtime.indirect_args_address));
	m_key.push_back(static_cast<uint32_t>(runtime.indirect_args_address >> 32u));
	m_key.push_back(static_cast<uint32_t>(runtime.workgroup_counts.size()));
	m_key.insert(m_key.end(), runtime.workgroup_counts.begin(), runtime.workgroup_counts.end());
	m_hash = HashWords(m_key);
}

ResourceMemo::Entry& ResourceMemo::Victim() {
	if (m_entries.size() < MaxEntries()) {
		return m_entries.emplace_back();
	}
	Bump(g_evicted);
	return *std::ranges::min_element(m_entries, {}, &Entry::used);
}

bool ResourceMemo::Materialize(const ResourcePlan& plan, const SrtRuntime& runtime,
                               ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	// Linked call sources read their code through source_reads, outside the log.
	if (!Enabled() || !plan.source_reads.empty()) {
		Bump(g_uncached);
		return MaterializeResources(plan, runtime, snapshot, specialization);
	}
	BuildKey(runtime);
	auto slot = m_entries.end();
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
		if (it->hash == m_hash && it->key == m_key) {
			slot = it;
			break;
		}
	}
	if (slot != m_entries.end()) {
		if (SrtReadLogMatches(slot->log, runtime)) {
			snapshot       = slot->snapshot;
			specialization = slot->specialization;
			// The snapshot carries all user data, including words the walk did not read.
			snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
			slot->used = ++m_clock;
			Bump(g_hits);
			return true;
		}
		Bump(g_stale);
	} else {
		Bump(g_misses);
		if (TraceEnabled() && !m_entries.empty()) TraceMiss();
	}
	bool ok = false;
	{
		const ScopedSrtReadLog scope(m_log, runtime);
		ok = MaterializeResources(plan, scope.Runtime(), snapshot, specialization);
	}
	if (!ok) return false;
	if (!m_log.complete) {
		if (slot != m_entries.end()) m_entries.erase(slot);
		return true;
	}
	// Keys hold the union of the words read so far: a walk that read a new word starts over.
	bool grew = false;
	if (m_user_data_words.size() < m_log.user_data_words.size()) {
		m_user_data_words.resize(m_log.user_data_words.size(), 0u);
	}
	for (size_t i = 0; i < m_log.user_data_words.size(); ++i) {
		grew |= (m_log.user_data_words[i] & ~m_user_data_words[i]) != 0u;
		m_user_data_words[i] |= m_log.user_data_words[i];
	}
	if (grew) {
		m_entries.clear();
		slot = m_entries.end();
		BuildKey(runtime);
	}
	auto& entry = slot != m_entries.end() ? *slot : Victim();
	entry.hash  = m_hash;
	entry.key   = m_key;
	std::swap(entry.log, m_log);
	entry.snapshot       = snapshot;
	entry.specialization = specialization;
	entry.used           = ++m_clock;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
