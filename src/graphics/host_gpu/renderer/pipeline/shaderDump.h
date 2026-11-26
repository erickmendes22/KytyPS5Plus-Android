#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDUMP_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDUMP_H_

#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>
#include <string>

namespace Libs::Graphics {

struct GraphicContext;

// Opt-in (--dump-shaders <dir>) capture of every host SPIR-V module and the exact Vulkan state of
// every pipeline built from them, so tools/shader_checker can recompile them offline on a device
// driver. Layout of <dir>:
//   device.json                 driver that produced the dump
//   spv/<stage>_<guest>_<content>.spv
//   pipelines/<kind>_<guests>_<content>.json   written before vkCreate*Pipelines
//   pipelines.log               "<record> <VkResult>" once the driver returned
namespace ShaderDump {

[[nodiscard]] bool Enabled();
[[nodiscard]] bool DsLightingCapture();
[[nodiscard]] bool DsDecisionCapture();
void RecordDsProbeGuestCode(uint64_t hash, std::span<const uint32_t> code,
                           const char* stage_name = "cs");

// Writes the module's SPIR-V (once per distinct content) and remembers the file for `module`.
void RecordModule(vk::ShaderModule module, const char* stage_name, uint64_t guest_hash,
                  std::span<const uint32_t> spirv, const std::string& original_file = {});
// F retains the original executable before inserting the diagnostic stores.
// This records bytes only; it never invents or registers a Vulkan module handle.
std::string RecordUninstrumentedModule(const char* stage_name, uint64_t guest_hash,
                                      std::span<const uint32_t> spirv);
// Must be called before a recorded module handle is destroyed, since handles are reused.
void ForgetModule(vk::ShaderModule module);
// Identity of the exact bound module, including diagnostic instrumentation.
// The name remains available on an I/O/budget failure, which is recorded in
// ds_prelight_shader_status.json; exporters must verify the file actually exists.
std::string ModuleFile(vk::ShaderModule module);
// Original executable paired with this exact bound permutation. An
// uninstrumented module is its own original; a missing handle stays missing.
std::string ModuleOriginalFile(vk::ShaderModule module);

using LayoutState = PipelineRecord::LayoutState;

// Returns the record name ("" when dumping is off), for failure messages and RecordResult.
std::string RecordComputePipeline(const GraphicContext& graphics,
                                  const vk::ComputePipelineCreateInfo& info,
                                  const LayoutState& layout, uint64_t guest_hash);
std::string RecordGraphicsPipeline(const GraphicContext& graphics,
                                   const vk::GraphicsPipelineCreateInfo& info,
                                   const LayoutState& layout,
                                   std::span<const uint64_t> guest_hashes);
void        RecordResult(const std::string& record, vk::Result result);

} // namespace ShaderDump
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDUMP_H_ */
