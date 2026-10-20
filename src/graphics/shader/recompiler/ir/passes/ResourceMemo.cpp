#include "graphics/shader/recompiler/ir/passes/ResourceMemo.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMemoRebase.h"

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
std::atomic<uint64_t> g_rebased {0};     // hits whose value sources changed (bases patched)
std::atomic<uint64_t> g_rebase_fail {0}; // hits that could not be rebased: walked
std::atomic<bool>     g_rebase {true};

// KYTY_SRT_MEMO_TRACE: per differing key word (user data 0..63, 64 = shader base, 65 = dispatch
// inputs, 66 = user-data size) how many misses differed there from the closest entry.
constexpr size_t TraceSlots = 67;
std::array<std::atomic<uint64_t>, TraceSlots> g_trace_words {};
std::array<std::atomic<uint64_t>, 5>          g_trace_distance {}; // 1, 2, 3, 4-7, 8+ words
// Walks that a content-keyed memo could have answered (KYTY_SRT_MEMO_TRACE): walks whose read
// bytes (kinds, sizes and values, not addresses) equal an earlier walk of the same source, and of
// those, how many also produced the same resources.
std::atomic<uint64_t> g_trace_walks {0};
std::atomic<uint64_t> g_trace_same_content {0};
std::atomic<uint64_t> g_trace_same_content_result {0};
std::atomic<uint64_t> g_trace_same_result {0};
// ...walks whose whole log (addresses too) equals an earlier walk's: the differing key words fed
// only descriptor values, not addresses or branches.
std::atomic<uint64_t> g_trace_same_log {0};
// ...of those, results equal once buffer/image base addresses are masked (rebasable by patching).
std::atomic<uint64_t> g_trace_same_log_masked {0};

uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
	return hash;
}

uint64_t ContentHash(const SrtReadLog& log) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const auto& entry: log.entries) {
		const uint32_t shape[2] = {static_cast<uint32_t>(entry.kind) | (entry.ok ? 0x100u : 0u), entry.bytes};
		hash = HashBytes(hash, shape, sizeof(shape));
	}
	return HashBytes(hash, log.values.data(), log.values.size());
}

uint64_t LogHash(const SrtReadLog& log) {
	uint64_t hash = ContentHash(log);
	for (const auto& entry: log.entries) hash = HashBytes(hash, &entry.address, sizeof(entry.address));
	return hash;
}

// Result hash without V# / T# base addresses (buffer dword0 + dword1[15:0], image dword0 +
// dword1[7:0]) and without the user-data copy.
uint64_t MaskedResultHash(const ResourceSnapshot& snapshot) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (auto value: snapshot.buffers) {
		value.dwords[0] = 0;
		value.dwords[1] &= ~0xffffu;
		hash = HashBytes(hash, value.dwords.data(), value.dword_count * 4u);
	}
	hash = HashBytes(hash, "|", 1);
	for (auto value: snapshot.images) {
		value.dwords[0] = 0;
		value.dwords[1] &= ~0xffu;
		hash = HashBytes(hash, value.dwords.data(), value.dword_count * 4u);
	}
	hash = HashBytes(hash, "|", 1);
	for (const auto& value: snapshot.samplers) hash = HashBytes(hash, value.dwords.data(), value.dword_count * 4u);
	return HashBytes(hash, snapshot.flattened_srt.data(), snapshot.flattened_srt.size() * 4u);
}

uint64_t ResultHash(const ResourceSnapshot& snapshot) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const auto* list: {&snapshot.buffers, &snapshot.images, &snapshot.samplers}) {
		for (const auto& value: *list) hash = HashBytes(hash, value.dwords.data(), value.dword_count * 4u);
		hash = HashBytes(hash, "|", 1);
	}
	return HashBytes(hash, snapshot.flattened_srt.data(), snapshot.flattened_srt.size() * 4u);
}
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

// Results kept per shader source (KYTY_SRT_MEMO_SLOTS / live knob memo_slots, default 256).
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
		return value != nullptr ? static_cast<size_t>(std::strtoul(value, nullptr, 10)) : size_t {256};
	}();
	return std::clamp<size_t>(slots, 1, 1024);
}

void ResourceMemo::SetSlots(size_t slots) {
	g_max_entries.store(std::clamp<size_t>(slots, 1, 1024), std::memory_order_relaxed);
}

void ResourceMemo::SetRebase(bool enabled) {
	g_rebase.store(enabled, std::memory_order_relaxed);
}

bool ResourceMemo::RebaseStartValue() {
	const char* value = std::getenv("KYTY_SRT_MEMO_REBASE");
	return value == nullptr || value[0] != '0';
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
	        .evicted  = g_evicted.exchange(0, std::memory_order_relaxed),
	        .rebased  = g_rebased.exchange(0, std::memory_order_relaxed),
	        .rebase_fail = g_rebase_fail.exchange(0, std::memory_order_relaxed)};
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
	const auto walks = g_trace_walks.exchange(0, std::memory_order_relaxed);
	std::snprintf(part, sizeof(part), " | walks=%llu", static_cast<unsigned long long>(walks));
	report += part;
	std::snprintf(part, sizeof(part), " same_content=%llu",
	              static_cast<unsigned long long>(g_trace_same_content.exchange(0, std::memory_order_relaxed)));
	report += part;
	std::snprintf(part, sizeof(part), " +same_result=%llu",
	              static_cast<unsigned long long>(g_trace_same_content_result.exchange(0, std::memory_order_relaxed)));
	report += part;
	std::snprintf(part, sizeof(part), " same_result=%llu",
	              static_cast<unsigned long long>(g_trace_same_result.exchange(0, std::memory_order_relaxed)));
	report += part;
	std::snprintf(part, sizeof(part), " same_log=%llu",
	              static_cast<unsigned long long>(g_trace_same_log.exchange(0, std::memory_order_relaxed)));
	report += part;
	std::snprintf(part, sizeof(part), " +masked_result=%llu",
	              static_cast<unsigned long long>(g_trace_same_log_masked.exchange(0, std::memory_order_relaxed)));
	report += part;
	report += " | closest differs in";
	const char* names[] = {"1", "2", "3", "4-7", "8+"};
	for (size_t i = 0; i < g_trace_distance.size(); ++i) {
		std::snprintf(part, sizeof(part), " %s:%llu", names[i],
		              static_cast<unsigned long long>(g_trace_distance[i].exchange(0, std::memory_order_relaxed)));
		report += part;
	}
	return report;
}

void ResourceMemo::TraceContent(const ResourceSnapshot& snapshot) {
	g_trace_walks.fetch_add(1, std::memory_order_relaxed);
	const auto content = ContentHash(m_log);
	const auto result  = ResultHash(snapshot);
	if (m_trace_content.size() > 8192) m_trace_content.clear();
	if (m_trace_results.size() > 8192) m_trace_results.clear();
	if (const auto found = m_trace_content.find(content); found != m_trace_content.end()) {
		g_trace_same_content.fetch_add(1, std::memory_order_relaxed);
		if (found->second == result) g_trace_same_content_result.fetch_add(1, std::memory_order_relaxed);
	}
	if (!m_trace_results.insert(result).second) g_trace_same_result.fetch_add(1, std::memory_order_relaxed);
	if (m_trace_logs.size() > 8192) m_trace_logs.clear();
	const auto masked = MaskedResultHash(snapshot);
	if (const auto [it, inserted] = m_trace_logs.try_emplace(LogHash(m_log), masked); !inserted) {
		g_trace_same_log.fetch_add(1, std::memory_order_relaxed);
		if (it->second == masked) g_trace_same_log_masked.fetch_add(1, std::memory_order_relaxed);
		it->second = masked;
	}
	m_trace_content[content] = result;
}

void ResourceMemo::TraceMiss() const {
	// Key layout (BuildKey): size, the masked user-data words in index order, base (2), dispatch.
	std::vector<size_t> slots;
	slots.push_back(66);
	for (size_t word = 0; word < m_user_data_words.size(); ++word) {
		for (auto bits = m_user_data_words[word]; bits != 0u; bits &= bits - 1u) {
			const auto index = word * 64u + static_cast<size_t>(std::countr_zero(bits));
			if (m_classes.IsValueWord(index)) continue;
			slots.push_back(std::min<size_t>(index, 63u));
		}
	}
	slots.push_back(64);
	slots.push_back(64);
	const Entry* closest = nullptr;
	size_t       best    = SIZE_MAX;
	for (const auto& entry: m_entries) {
		if (entry.key.size() != m_key.size()) continue; // also skips forgotten entries
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
			if (m_classes.IsValueWord(index)) continue; // rebased on a hit instead
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

uint32_t ResourceMemo::Victim() {
	if (m_entries.size() < MaxEntries()) {
		m_entries.emplace_back();
		return static_cast<uint32_t>(m_entries.size() - 1u);
	}
	// Clock (second chance): an entry hit since the hand last passed it stays one more round.
	Bump(g_evicted);
	for (;;) {
		if (m_hand >= m_entries.size()) m_hand = 0;
		auto& entry = m_entries[m_hand];
		const auto index = m_hand++;
		if (entry.referenced) {
			entry.referenced = false;
			continue;
		}
		Forget(index);
		return index;
	}
}

void ResourceMemo::Forget(uint32_t index) {
	auto& entry = m_entries[index];
	if (const auto found = m_index.find(entry.hash); found != m_index.end() && found->second == index) {
		m_index.erase(found);
	}
	entry.key.clear();
	entry.referenced = false;
}

bool ResourceMemo::Materialize(const ResourcePlan& plan, const SrtRuntime& runtime,
                               ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	// Linked call sources read their code through source_reads, outside the log.
	if (!Enabled() || !plan.source_reads.empty()) {
		Bump(g_uncached);
		return MaterializeResources(plan, runtime, snapshot, specialization);
	}
	// Rebase on/off changes the key layout: start over when it flips.
	const bool rebase = g_rebase.load(std::memory_order_relaxed);
	if (!m_classified || rebase != m_rebase) {
		m_classes    = rebase ? ClassifyUserData(plan) : UserDataClasses {};
		m_classified = true;
		m_rebase     = rebase;
		m_entries.clear();
		m_index.clear();
		m_hand = 0;
	}
	BuildKey(runtime);
	uint32_t slot = NoSlot;
	if (const auto found = m_index.find(m_hash); found != m_index.end() && m_entries[found->second].key == m_key) {
		slot = found->second;
	}
	if (slot != NoSlot) {
		auto& entry = m_entries[slot];
		if (SrtReadLogMatches(entry.log, runtime)) {
			snapshot       = entry.snapshot;
			specialization = entry.specialization;
			// The snapshot carries all user data, including words the walk did not read.
			snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
			const auto rebased = Rebase(plan, runtime, entry, snapshot);
			if (rebased != RebaseResult::Failed) {
				entry.referenced = true;
				Bump(g_hits);
				if (rebased == RebaseResult::Patched) Bump(g_rebased);
				return true;
			}
			Bump(g_rebase_fail);
		} else {
			Bump(g_stale);
		}
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
		if (slot != NoSlot) Forget(slot);
		return true;
	}
	if (TraceEnabled()) TraceContent(snapshot);
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
		m_index.clear();
		m_hand = 0;
		slot   = NoSlot;
		BuildKey(runtime);
	}
	if (slot == NoSlot) {
		slot = Victim();
		// A different key with the same hash loses its index entry (it can no longer be found).
		if (const auto found = m_index.find(m_hash); found != m_index.end()) Forget(found->second);
		m_index[m_hash] = slot;
	}
	auto& entry = m_entries[slot];
	entry.hash  = m_hash;
	entry.key   = m_key;
	std::swap(entry.log, m_log);
	entry.snapshot       = snapshot;
	entry.specialization = specialization;
	entry.referenced     = false;
	if (!m_classes.Empty()) {
		// The raw values of the value sources this walk produced, to rebase from on a hit.
		entry.value_active.assign(plan.active_sources.begin(), plan.active_sources.end());
		if (!EvaluateValueSources(plan, runtime, m_classes.value_sources, entry.value_active, entry.value_raw)) {
			Forget(slot);
		}
	}
	return true;
}

ResourceMemo::RebaseResult ResourceMemo::Rebase(const ResourcePlan& plan, const SrtRuntime& runtime,
                                                const Entry& entry, ResourceSnapshot& snapshot) {
	if (m_classes.Empty()) return RebaseResult::Same;
	if (!EvaluateValueSources(plan, runtime, m_classes.value_sources, entry.value_active, m_current)) {
		return RebaseResult::Failed; // counted as rebase_fail; rare (the reads were just validated)
	}
	if (m_current == entry.value_raw) return RebaseResult::Same;
	return RebaseSnapshot(plan, m_classes.value_sources, entry.value_raw, m_current, snapshot)
	           ? RebaseResult::Patched
	           : RebaseResult::Failed;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
