#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::DemonsMaterialCoverage {

// The recompiler emits OpKill in the fragment entry point's return path.
// With attachment writes disabled, replacing it with OpReturn makes the
// same fragments reach late depth testing without changing the vertex path.
[[nodiscard]] inline std::vector<uint32_t> ReplayShader(std::span<const uint32_t> original,
                                                       bool keep_discard) {
	constexpr size_t header_words = 5;
	if (original.size() < header_words || original.front() != spv::MagicNumber) return {};
	std::vector<uint32_t> result(original.begin(), original.begin() + header_words);
	result.reserve(original.size());
	bool has_kill = false;
	for (size_t offset = header_words; offset < original.size();) {
		const auto size = original[offset] >> 16u;
		if (size == 0 || size > original.size() - offset) return {};
		const auto op = original[offset] & 0xffffu;
		const auto words = original.subspan(offset, size);
		offset += size;
		if (op == spv::OpTerminateInvocation || op == spv::OpDemoteToHelperInvocation) return {};
		if (op == spv::OpExecutionMode && size == 3 &&
		    words[2] == spv::ExecutionModeEarlyFragmentTests) continue;
		if (op == spv::OpKill) {
			if (size != 1) return {};
			has_kill = true;
			result.push_back((1u << 16u) | (keep_discard ? spv::OpKill : spv::OpReturn));
		} else {
			result.insert(result.end(), words.begin(), words.end());
		}
	}
	if (!has_kill) result.clear();
	return result;
}

} // namespace Libs::Graphics::DemonsMaterialCoverage
