#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "GpuDiagnosticsTests: failed: %s\n", text);
		std::abort();
	}
}

void TestParseSpec() {
	const auto list = PipelineStats::ParseSpec("098883a6,0x48BC4EF0,,zz,12345678901234567");
	Check(list.size() == 2, "two valid prefixes");
	Check(list[0] == "098883a6", "first prefix kept");
	Check(list[1] == "48bc4ef0", "0x stripped and lower-cased");
	const auto all = PipelineStats::ParseSpec("all");
	Check(all.size() == 1 && all[0].empty(), "all matches every hash");
	Check(PipelineStats::ParseSpec("").empty(), "empty spec");
	const auto colon = PipelineStats::ParseSpec("338b:c739+0078");
	Check(colon.size() == 3 && colon[2] == "0078", "':' and '+' separate");
}

void TestMatches() {
	const std::vector<std::string> prefixes {"098883a6", "48bc"};
	Check(PipelineStats::Matches(prefixes, 0x098883a6f393aa10ull), "full prefix");
	Check(PipelineStats::Matches(prefixes, 0x48bc4ef05ebce9d4ull), "short prefix");
	Check(!PipelineStats::Matches(prefixes, 0x338b450551457250ull), "other hash");
	Check(PipelineStats::Matches(prefixes, 0x098883a6f393aa10ull), "leading zero digits kept");
	const std::vector<std::string> all {std::string {}};
	Check(PipelineStats::Matches(all, 1), "all");
}

} // namespace

int main() {
	TestParseSpec();
	TestMatches();
	std::puts("GpuDiagnosticsTests: ok");
	return 0;
}
