#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTATS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// Opt-in driver statistics for chosen pipelines (VK_KHR_pipeline_executable_properties).
//
//   KYTY_PIPELINE_STATS=<hex>[:<hex>...]   guest shader hash prefixes (',' '+' ':' separate), or "all"
//
// A pipeline with a guest stage matching one prefix is created with the capture flags, and its
// executables' statistics (registers, waves, instruction counts, spills, subgroup size) and
// internal representations (Turnip: final NIR and ir3 disassembly) are appended to
// _perf_pipestats.txt. Unset: no extension, no flags, no cost.
namespace PipelineStats {

// Parses the KYTY_PIPELINE_STATS value: lower-case hex prefixes, "all" -> {""}. Bad items are
// skipped. Exposed for tests.
[[nodiscard]] std::vector<std::string> ParseSpec(std::string_view spec);
// Whether a 16-digit hash matches one of the prefixes.
[[nodiscard]] bool Matches(std::span<const std::string> prefixes, uint64_t guest_hash);

// The environment asks for statistics (decides whether the device enables the extension).
[[nodiscard]] bool Requested();
// The device enabled pipelineExecutableInfo and one of `guest_hashes` was asked for.
[[nodiscard]] bool Wants(const GraphicContext& graphics, std::span<const uint64_t> guest_hashes);
[[nodiscard]] constexpr vk::PipelineCreateFlags CaptureFlags() {
	return vk::PipelineCreateFlagBits::eCaptureStatisticsKHR |
	       vk::PipelineCreateFlagBits::eCaptureInternalRepresentationsKHR;
}
// Appends the pipeline's statistics and representations under `label`.
void Write(const GraphicContext& graphics, vk::Pipeline pipeline, const std::string& label);

} // namespace PipelineStats
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTATS_H_ */
