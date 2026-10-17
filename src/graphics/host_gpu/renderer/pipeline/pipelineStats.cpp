#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"

#include "graphics/host_gpu/graphicContext.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <fmt/format.h>
#include <mutex>

namespace Libs::Graphics::PipelineStats {

namespace {

constexpr size_t kMaxRepresentationBytes = 512u * 1024u;
constexpr const char* kFileName          = "_perf_pipestats.txt";

const std::vector<std::string>& Prefixes() {
	static const std::vector<std::string> prefixes = [] {
		const char* spec = std::getenv("KYTY_PIPELINE_STATS");
		return spec != nullptr ? ParseSpec(spec) : std::vector<std::string> {};
	}();
	return prefixes;
}

std::string StatisticValue(const vk::PipelineExecutableStatisticKHR& stat) {
	switch (stat.format) {
		case vk::PipelineExecutableStatisticFormatKHR::eBool32:
			return stat.value.b32 != VK_FALSE ? "true" : "false";
		case vk::PipelineExecutableStatisticFormatKHR::eInt64:
			return fmt::format("{}", stat.value.i64);
		case vk::PipelineExecutableStatisticFormatKHR::eUint64:
			return fmt::format("{}", stat.value.u64);
		case vk::PipelineExecutableStatisticFormatKHR::eFloat64:
			return fmt::format("{:.3f}", stat.value.f64);
		default: return "?";
	}
}

void WriteStatistics(std::FILE* out, const GraphicContext& graphics,
                     const vk::PipelineExecutableInfoKHR& executable) {
	uint32_t count = 0;
	if (graphics.device.getPipelineExecutableStatisticsKHR(&executable, &count, nullptr) !=
	    vk::Result::eSuccess) {
		std::fprintf(out, "  statistics: query failed\n");
		return;
	}
	std::vector<vk::PipelineExecutableStatisticKHR> stats(count);
	if (graphics.device.getPipelineExecutableStatisticsKHR(&executable, &count, stats.data()) !=
	    vk::Result::eSuccess) {
		std::fprintf(out, "  statistics: query failed\n");
		return;
	}
	for (const auto& stat: stats) {
		std::fprintf(out, "  %s = %s\n", stat.name.data(), StatisticValue(stat).c_str());
	}
}

void WriteRepresentations(std::FILE* out, const GraphicContext& graphics,
                          const vk::PipelineExecutableInfoKHR& executable) {
	uint32_t count = 0;
	if (graphics.device.getPipelineExecutableInternalRepresentationsKHR(&executable, &count,
	                                                                    nullptr) !=
	    vk::Result::eSuccess) {
		return;
	}
	std::vector<vk::PipelineExecutableInternalRepresentationKHR> reps(count);
	if (graphics.device.getPipelineExecutableInternalRepresentationsKHR(&executable, &count,
	                                                                    reps.data()) !=
	    vk::Result::eSuccess) {
		return;
	}
	std::vector<std::string> texts(count);
	for (uint32_t i = 0; i < count; i++) {
		texts[i].assign(std::min(reps[i].dataSize, kMaxRepresentationBytes), '\0');
		reps[i].dataSize = texts[i].size();
		reps[i].pData    = texts[i].empty() ? nullptr : texts[i].data();
	}
	const auto result = graphics.device.getPipelineExecutableInternalRepresentationsKHR(
	    &executable, &count, reps.data());
	// eIncomplete only means a representation was cut at kMaxRepresentationBytes.
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!reps[i].isText || texts[i].empty()) {
			continue;
		}
		const auto length = std::char_traits<char>::length(texts[i].c_str());
		std::fprintf(out, "  --- %s ---\n%.*s\n", reps[i].name.data(), static_cast<int>(length),
		             texts[i].c_str());
	}
}

} // namespace

std::vector<std::string> ParseSpec(std::string_view spec) {
	std::vector<std::string> prefixes;
	size_t                   start = 0;
	while (start <= spec.size()) {
		// ',' cannot pass through --env on Android (the launcher splits arguments on it).
		const auto end  = std::min(spec.find_first_of(",:+", start), spec.size());
		auto       item = spec.substr(start, end - start);
		if (item.starts_with("0x") || item.starts_with("0X")) {
			item.remove_prefix(2);
		}
		std::string prefix;
		bool        valid = !item.empty() && item.size() <= 16;
		for (const char c: item) {
			valid = valid && std::isxdigit(static_cast<unsigned char>(c)) != 0;
			prefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		if (item == "all") {
			return {std::string {}};
		}
		if (valid) {
			prefixes.push_back(std::move(prefix));
		}
		start = end + 1;
	}
	return prefixes;
}

bool Matches(std::span<const std::string> prefixes, uint64_t guest_hash) {
	char hex[17];
	std::snprintf(hex, sizeof(hex), "%016" PRIx64, guest_hash);
	const std::string_view text(hex, 16);
	for (const auto& prefix: prefixes) {
		if (text.starts_with(prefix)) {
			return true;
		}
	}
	return false;
}

bool Requested() {
	return !Prefixes().empty();
}

bool Wants(const GraphicContext& graphics, std::span<const uint64_t> guest_hashes) {
	if (!graphics.pipeline_executable_info_enabled || !Requested()) {
		return false;
	}
	for (const auto hash: guest_hashes) {
		if (Matches(Prefixes(), hash)) {
			return true;
		}
	}
	return false;
}

void Write(const GraphicContext& graphics, vk::Pipeline pipeline, const std::string& label) {
	if (pipeline == nullptr) {
		return;
	}
	static std::mutex mutex;
	std::scoped_lock  lock(mutex);
	static std::FILE* out = std::fopen(kFileName, "w");
	if (out == nullptr) {
		return;
	}
	vk::PipelineInfoKHR info {};
	info.pipeline  = pipeline;
	uint32_t count = 0;
	if (graphics.device.getPipelineExecutablePropertiesKHR(&info, &count, nullptr) !=
	    vk::Result::eSuccess) {
		std::fprintf(out, "=== %s: executable query failed\n", label.c_str());
		return;
	}
	std::vector<vk::PipelineExecutablePropertiesKHR> executables(count);
	if (graphics.device.getPipelineExecutablePropertiesKHR(&info, &count, executables.data()) !=
	    vk::Result::eSuccess) {
		std::fprintf(out, "=== %s: executable query failed\n", label.c_str());
		return;
	}
	std::fprintf(out, "=== %s: %u executables\n", label.c_str(), count);
	for (uint32_t i = 0; i < count; i++) {
		const auto& props = executables[i];
		std::fprintf(out, "## %s [%s] stages=0x%x subgroup=%u\n", props.name.data(),
		             props.description.data(), static_cast<uint32_t>(props.stages),
		             props.subgroupSize);
		vk::PipelineExecutableInfoKHR executable {};
		executable.pipeline        = pipeline;
		executable.executableIndex = i;
		WriteStatistics(out, graphics, executable);
		WriteRepresentations(out, graphics, executable);
	}
	std::fflush(out);
}

} // namespace Libs::Graphics::PipelineStats
