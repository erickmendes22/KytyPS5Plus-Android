#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMO_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMO_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// KYTY_SRT_MEMO (live knob srt_memo): memoized MaterializeResources for one shader source.
//
// A materialization is a pure function of its plan, the user-data words it reads, the shader
// base, the dispatch inputs and the guest memory it reads (SrtReadLog). A result is reused when
// those user-data words and inputs are equal (the key) and re-reading every logged access returns
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
	};

	// Same contract as MaterializeResources.
	bool Materialize(const ResourcePlan& plan, const SrtRuntime& runtime, ResourceSnapshot& snapshot,
	                 ResourceSpecialization& specialization);

	[[nodiscard]] size_t Size() const { return m_entries.size(); }

	static void  SetEnabled(bool enabled);
	static bool  Enabled();
	// KYTY_SRT_MEMO start value (default off).
	static bool  StartValue();
	// Results kept per source: KYTY_SRT_MEMO_SLOTS (default 32), SetSlots (1..1024) overrides.
	static size_t SlotsStartValue();
	static void   SetSlots(size_t slots);
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
		uint64_t               used = 0;
	};

	void   BuildKey(const SrtRuntime& runtime);
	void   TraceMiss() const;
	Entry& Victim();

	// Union of the user-data words the walks of this source read; keys hold these words only.
	std::vector<uint64_t>  m_user_data_words;
	std::vector<Entry>     m_entries;
	std::vector<uint32_t>  m_key;
	uint64_t               m_hash  = 0;
	uint64_t               m_clock = 0;
	SrtReadLog             m_log;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMO_H_ */
