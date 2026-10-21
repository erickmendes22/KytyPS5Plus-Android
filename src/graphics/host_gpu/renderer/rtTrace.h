#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RTTRACE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RTTRACE_H_

#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <span>

// KYTY_RT_TRACE=<hex prefixes> (diagnostics, opt-in): for the first six dispatches/draws of a
// matching shader, logs every image it binds ("[rt-trace] src ..."): address, size, extent,
// format, levels/layers, tile mode, storage or sampled. The images it writes are then followed
// through the next bindings of every shader ("[rt-trace] use +N ..."): which later passes read or
// write them, and whether a reader also writes one (a temporal accumulation shape). Lines go to
// the perf log.
namespace Libs::Graphics::RtTrace {

void OnStageBindings(uint64_t shader_hash, ShaderType stage, std::span<const TextureBinding> images);

} // namespace Libs::Graphics::RtTrace

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RTTRACE_H_
