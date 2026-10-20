#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_FTZPROBE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_FTZPROBE_H_

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

// KYTY_FTZ32=1 (graphics/shader/ftz32.h): when the device supports the 32-bit DenormFlushToZero
// execution mode independently of the 16-bit DenormPreserve the recompiler may declare, runs
// gpu_ftz_probe.comp once under that mode and returns the Ftz32::Op bits whose results match GCN
// (their explicit flushes can go). 0 when the option is off, unsupported or the check fails.
// Logs the outcome.
[[nodiscard]] uint32_t ProbeFtz32Ops(GraphicContext& graphics);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_FTZPROBE_H_
