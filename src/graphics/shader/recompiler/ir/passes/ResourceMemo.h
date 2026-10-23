#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMO_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMO_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMemoRebase.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// KYTY_SRT_MEMO (live knob srt_memo): memoized MaterializeResources for one shader source.
//
// A materialization is a pure function of its plan, the user-data words it reads, the shader
// base, the dispatch inputs and the guest memory it reads (SrtReadLog). A result is reused when
// those user-data words and inputs are equal (the key, looked up by hash) and re-reading every logged access returns
// the logged bytes, so a descriptor the guest or the GPU rewrote since is re-read and the walk
// runs again: there is no invalidation to miss. The re-read goes through the same readers as the
// walk (GPU-written bytes still read back first), it only skips the evaluation.
class ResourceMemo {
public:
	struct Stats {
		uint64_t hits     = 0; // reused after re-reading
		uint64_t misses   = 0; // no entry for the key: walked
		uint64_t stale    = 0; // entry for the key, but a re-read differed: walked
		uint64_t uncached = 0; // memo off, or an input the log cannot hold: walked, not kept
		uint64_t evicted  = 0; // entries replaced by a new key (slots full)
		uint64_t rebased  = 0; // hits whose base addresses were patched (ResourceMemoRebase.h)
		uint64_t rebase_fail = 0; // hits that could not be rebased: walked
	};

	// Same contract as MaterializeResources.
	bool Materialize(const ResourcePlan& plan, const SrtRuntime& runtime, ResourceSnapshot& snapshot,
	                 ResourceSpecialization& specialization);

	[[nodiscard]] size_t Size() const { return m_entries.size(); }

	static void  SetEnabled(bool enabled);
	static bool  Enabled();
	// Results kept per source: KYTY_SRT_MEMO_SLOTS (default 256), SetSlots (1..1024) overrides.
	static size_t SlotsStartValue();
	static void   SetSlots(size_t slots);
	// KYTY_SRT_MEMO_REBASE (default on; live knob memo_rebase): key only on user-data words
	// that reach addresses or branches, and patch base addresses on a hit.
	static void   SetRebase(bool enabled);
	static bool   RebaseStartValue();
	// Counters over every memo since the last call (one thread uses memos: the GPU thread).
	static Stats TakeStats();
	// KYTY_SRT_MEMO_TRACE=1: why keys miss. For each miss with entries present, the user-data
	// words (or "base"/"dispatch") in which the closest entry's key differs; a summary of the
	// most frequent ones since the last call ("" when tracing is off or nothing missed).
	static std::string TakeMissReport();

private:
	struct Entry {
		uint64_t               hash = 0;
		std::vector<uint32_t>  key;
		SrtReadLog             log;
		ResourceSnapshot       snapshot;
		ResourceSpecialization specialization;
		bool                   referenced = false; // hit since the clock hand last passed
		std::vector<DescriptorValue> value_raw;    // raw values of the value sources
		std::vector<uint8_t>         value_active; // active sources of the walk (empty = all)
	};
	enum class RebaseResult { Same, Patched, Failed };
	RebaseResult Rebase(const ResourcePlan& plan, const SrtRuntime& runtime, const Entry& entry,
	                    ResourceSnapshot& snapshot);
	static constexpr uint32_t NoSlot = UINT32_MAX;

	void   BuildKey(const SrtRuntime& runtime);
	void   TraceMiss() const;
	void   TraceContent(const ResourceSnapshot& snapshot);
	// A free slot: a new one below the limit, else the clock's choice (unindexed first).
	uint32_t Victim();
	void     Forget(uint32_t index);

	// Union of the user-data words the walks of this source read; keys hold these words only.
	std::vector<uint64_t>  m_user_data_words;
	std::vector<Entry>     m_entries;
	// Key hash -> slot. A hash collision between two live keys keeps only the newer one.
	std::unordered_map<uint64_t, uint32_t> m_index;
	uint32_t               m_hand  = 0;
	std::vector<uint32_t>  m_key;
	uint64_t               m_hash  = 0;
	SrtReadLog             m_log;
	UserDataClasses        m_classes;
	bool                   m_classified = false;
	bool                   m_rebase     = false;
	std::vector<DescriptorValue> m_current;
	// KYTY_SRT_MEMO_TRACE only: read-content hash -> result hash, and result hashes seen.
	std::unordered_map<uint64_t, uint64_t> m_trace_content;
	std::unordered_set<uint64_t>           m_trace_results;
	std::unordered_map<uint64_t, uint64_t> m_trace_logs; // log hash -> masked result hash
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMO_H_ */
