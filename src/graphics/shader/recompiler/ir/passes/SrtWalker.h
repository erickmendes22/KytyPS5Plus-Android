#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

// KYTY_FAULT_TRACE: what the specialization is evaluating on this thread (a static string or
// nullptr), so a readback it forces can be attributed (kernel/memory.cpp prints it as site=).
const char*& SpecializationTraceSite();
class ScopedSpecializationSite {
public:
	explicit ScopedSpecializationSite(const char* site): m_previous(SpecializationTraceSite()) {
		SpecializationTraceSite() = site;
	}
	~ScopedSpecializationSite() { SpecializationTraceSite() = m_previous; }
	ScopedSpecializationSite(const ScopedSpecializationSite&)            = delete;
	ScopedSpecializationSite& operator=(const ScopedSpecializationSite&) = delete;

private:
	const char* m_previous;
};

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);
// Host copy of guest memory for ordinary (non-vector) SRT reads that have no reader. Default:
// memcpy from the guest address (a read fault on a GPU-written page reads back the whole page).
using SrtHostRead = void (*)(uint64_t address, void* data, size_t size);
void SetSrtHostRead(SrtHostRead read);
// A walk thread's own host read (ScopedSrtThreadReader): false when the bytes cannot be read safely
// on this thread (e.g. GPU-written, needing the GPU thread's readback); the walk then reads zeros
// and the guard reports the failure, so the caller materializes on the GPU thread instead.
using SrtThreadReader = bool (*)(void* userdata, uint64_t address, void* data, size_t size);

// Reports whether [address, address + size) is mapped guest memory.
using SrtAddressProbe = bool (*)(uint64_t address, uint64_t size);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	std::span<const uint32_t> workgroup_counts;
	// Optional. Host evaluation also reads blocks the GPU may skip (EXEC-masked branches), so
	// a scalar address there can be unmapped (e.g. a null pointer the guest never follows).
	// Such a read yields zero instead of a host fault, as an AMD GPU's unmapped-page read does.
	SrtAddressProbe           mapped                     = nullptr;
	// Optional. Reads for block (branch) conditions only: fails instead of waiting for GPU-written
	// memory. Such a condition stays unknown and both successors count as reachable, which is
	// safe because the shader still branches at run time; it only widens the collected resources.
	SrtMemoryReader           read_branch_memory         = nullptr;
	uint64_t                  indirect_args_address = 0;
};

// Every input one resource materialization took from outside its plan, in order: guest memory
// accesses (host reads, the runtime's readers and mapping probes, each with its result) and the
// user-data words it used. The walk is a pure function of these, so a later materialization with
// equal user-data words whose re-reads all return the logged values would produce the same
// resources (pipeline/resourceMemo.h). Reads are recorded on the walking thread only.
struct SrtReadLog {
	enum class Kind : uint8_t { Host, Spec, Branch, Mapped };
	struct Entry {
		uint64_t address      = 0;
		uint32_t bytes        = 0;
		uint32_t value_offset = 0; // into values (Host/Spec/Branch with ok)
		Kind     kind         = Kind::Host;
		bool     ok           = true;
	};
	static constexpr size_t MaxValueBytes = 16 * 1024;

	std::vector<Entry>    entries;
	std::vector<uint8_t>  values;
	std::vector<uint64_t> user_data_words; // bit i: user data word i was read
	bool                  complete = true;  // false: too large to keep (nothing can be reused)
	// Per kind: the entry a contiguous access extends (logging scratch).
	std::array<uint32_t, 4> last_entry {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};

	void Clear();
	void NoteUserData(size_t index);
};

// Records the current thread's SRT accesses into `log` while alive. Readers of the runtime only
// log when they come from LoggingRuntime().
class ScopedSrtReadLog {
public:
	ScopedSrtReadLog(SrtReadLog& log, const SrtRuntime& source);
	~ScopedSrtReadLog();
	ScopedSrtReadLog(const ScopedSrtReadLog&)            = delete;
	ScopedSrtReadLog& operator=(const ScopedSrtReadLog&) = delete;

	// `source` with its readers and mapping probe replaced by logging wrappers around them.
	[[nodiscard]] SrtRuntime Runtime() const;

private:
	SrtReadLog*       m_previous_log;
	const SrtRuntime* m_previous_source;
	SrtRuntime        m_source;
};

// Re-reads every logged access, in order, through `runtime` (the unwrapped runtime of the logged
// walk) and the SRT host read. True when each one returns what the log holds.
[[nodiscard]] bool SrtReadLogMatches(const SrtReadLog& log, const SrtRuntime& runtime);

// Routes this thread's SRT host reads (native walk, ordinary raw reads) through `reader` while
// alive, instead of SetSrtHostRead's process-wide reader.
// Whether a ScopedSrtThreadReader is active on this thread (a worker walk: failures are expected
// and reported to the caller, not logged).
[[nodiscard]] bool SrtThreadReaderActive();

class ScopedSrtThreadReader {
public:
	ScopedSrtThreadReader(SrtThreadReader reader, void* userdata);
	~ScopedSrtThreadReader();
	ScopedSrtThreadReader(const ScopedSrtThreadReader&)            = delete;
	ScopedSrtThreadReader& operator=(const ScopedSrtThreadReader&) = delete;
	// Whether a read through `reader` failed since the guard was created.
	[[nodiscard]] bool Failed() const;

private:
	SrtThreadReader m_previous_reader;
	void*           m_previous_userdata;
	bool            m_previous_failed;
};

class SrtReadCapture {
public:
	SrtReadCapture(SrtRuntime source, std::vector<std::pair<uint64_t, uint64_t>>& ranges):
	    m_source(source), m_ranges(ranges) {}
	SrtRuntime ObservedRuntime();
	// True when the runtime's ordinary reads copy host memory directly.
	static bool ReadsHostMemory(const SrtRuntime& runtime);

private:
	static bool ReadStrict(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadOrdinary(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadBranch(void* userdata, uint64_t address, std::span<uint32_t> values);

	SrtRuntime                                  m_source;
	std::vector<std::pair<uint64_t, uint64_t>>& m_ranges;
};

// Retained reads no longer need the guest instruction PC. A clean read evaluates
// its address and bounds through the strict reader as well as the final DWORD.
struct SrtReadFlags {
	uint32_t index = 0;
	uint32_t clean = 0;
};

enum class RuntimeValueType { Any, Integer, ImmutableInteger };

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uniform across a compute wave, including values derived from its workgroup ID.
// This does not imply that the host can evaluate the value.
bool ValidateWaveUniformValue(const ResourcePlan& program, Value value);
void CompileSrtWalker(ResourcePlan& program);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);
// Reads every value through read_branch_memory (requires it).
SrtRuntime BranchRuntime(SrtRuntime runtime);

// Nonnegative affine coefficients: element zero is constant, remaining elements are
// caller-selected leaves. Runtime range proofs must additionally exclude U32 wrap.
template <size_t Size>
std::optional<std::array<uint64_t, Size>> AffineIndex(
    Value value, const auto& resolve, const auto& leaf, uint32_t depth = 0) {
	value = resolve(value);
	if (depth > 32u || value.GetType() != Type::U32) return {};
	std::array<uint64_t, Size> result {};
	if (value.IsImmediate()) {
		result[0] = value.U32();
		return result;
	}
	if (const auto index = leaf(value)) {
		result[*index] = 1u;
		return result;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return {};
	const auto op = inst->GetOpcode();
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) return {};
	auto left = AffineIndex<Size>(inst->Arg(0), resolve, leaf, depth + 1u);
	auto right = AffineIndex<Size>(inst->Arg(1), resolve, leaf, depth + 1u);
	if (!left || !right) return {};
	const auto constant = [](const auto& coefficients) {
		return std::ranges::all_of(std::span(coefficients).subspan(1u),
		                          [](uint64_t coefficient) { return coefficient == 0u; });
	};
	if (op == ValueOpcode::IMul32 && !constant(*right)) std::swap(left, right);
	if (op != ValueOpcode::IAdd32 && !constant(*right)) return {};
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32u) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (size_t index = 0; index < Size; ++index) {
		(*left)[index] = op == ValueOpcode::IAdd32
		    ? (*left)[index] + (*right)[index] : (*left)[index] * (*right)[0];
		if ((*left)[index] > UINT32_MAX) return {};
	}
	return left;
}

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          SrtWalker* clean_evaluator = nullptr);
	// A session on `scratch` instead of the plan's own (concurrent walks of one plan).
	SrtWalker(const ResourcePlan& program, ResourcePlan::Scratch& scratch, const SrtRuntime& runtime,
	          SrtWalker* clean_evaluator = nullptr);
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);
	[[nodiscard]] ResourcePlan::Scratch& WalkScratch() const { return m_scratch; }

private:
	SrtWalker(SrtWalker& parent, Value active_mask, bool clean = false,
	          SrtWalker* clean_evaluator = nullptr);
	ResourcePlan::EvaluationContext& AcquireContext();
	void ResetUnknowns();
	static float Float32(uint64_t bits);
	bool EvaluateWide(Value value, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result);
	bool EvaluateInst(const Inst& inst, uint64_t& result);

	const ResourcePlan&              m_program;
	ResourcePlan::Scratch&           m_scratch;
	SrtRuntime                      m_runtime;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	const std::vector<uint8_t>*      m_reachable_blocks = nullptr;
	const SrtWalker*                m_session;
	uint8_t                         m_read_policy;
	ResourcePlan::EvaluationContext& m_context;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
