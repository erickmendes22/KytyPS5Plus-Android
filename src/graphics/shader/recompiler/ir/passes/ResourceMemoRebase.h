#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMOREBASE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMOREBASE_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Rebasable memo hits (ResourceMemo). Most walks that miss the memo read exactly the same memory
// as an earlier walk of the same source and differ only in user-data words that end up in buffer
// or image base addresses (per-draw constant buffers from a ring). Such "value" words are left out
// of the memo key; on a hit the descriptor sources that use them are evaluated again and only
// their base addresses are patched into the kept resources.
struct UserDataClasses {
	// User-data words reached only from plain descriptor dwords through pure arithmetic (never
	// from an address, a branch, an indirect descriptor, a sampler or a written resource).
	std::vector<uint64_t> value_words;
	// Non-indirect descriptor sources whose dwords use a value word, ascending.
	std::vector<uint32_t> value_sources;

	[[nodiscard]] bool Empty() const { return value_sources.empty(); }
	[[nodiscard]] bool IsValueWord(size_t index) const {
		return index / 64u < value_words.size() && ((value_words[index / 64u] >> (index % 64u)) & 1u) != 0u;
	}
};

[[nodiscard]] UserDataClasses ClassifyUserData(const ResourcePlan& plan);

// The raw descriptor values of `sources` for `runtime`, as MaterializeResources evaluates them;
// a source inactive in `active` (the plan's active_sources after a walk; empty = all active)
// yields an empty value. False when an evaluation fails.
[[nodiscard]] bool EvaluateValueSources(const ResourcePlan& plan, const SrtRuntime& runtime,
                                        std::span<const uint32_t> sources, std::span<const uint8_t> active,
                                        std::vector<DescriptorValue>& values);

// Why RebaseSnapshot refused (counted; TakeRebaseFailures returns and clears the counts).
enum class RebaseFailure : uint8_t {
	Normalized,  // the kept resource is not the raw value (zeroed as invalid)
	NotBaseOnly, // a changed source differs outside the base address bits
	Null,        // the base became or stopped being null
	Uniform,     // the uniform-buffer specialization (16-byte alignment) would change
	Sampler,     // a sampler uses a changed source
	Evaluate,    // re-evaluating a value source failed
	Count
};
[[nodiscard]] std::array<uint64_t, static_cast<size_t>(RebaseFailure::Count)> TakeRebaseFailures();

// Moves the resources of a kept snapshot from the `cached` raw values of `sources` to `current`.
// Only base addresses may differ, and only where the specialization cannot see it (same 16-byte
// uniform-buffer specialization, same null-ness); every resource using a changed source must be a direct
// buffer or image whose kept value is the raw value. False when the snapshot cannot be rebased
// (the caller walks instead).
[[nodiscard]] bool RebaseSnapshot(const ResourcePlan& plan, std::span<const uint32_t> sources,
                                  std::span<const DescriptorValue> cached,
                                  std::span<const DescriptorValue> current, ResourceSnapshot& snapshot);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMEMOREBASE_H_ */
