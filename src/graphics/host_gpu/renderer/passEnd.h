#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PASSEND_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PASSEND_H_

#include <cstdint>

namespace Libs::Graphics {

// Why a render pass is being ended; --perf-stats counts passes (with draws) per reason.
enum class PassEnd : uint8_t {
	State,       // next draw targets different attachments
	Transit,     // image barrier for a sampled/stored image
	Attach,      // layout transition of an attachment being acquired
	ImageCopy,   // image upload/download/copy/blit/resolve/clear
	Buffer,      // buffer upload or download
	Compute,     // compute dispatch (incl. mesh ring reuse)
	ShaderWrite, // shader buffer writes or GDS barriers
	Submit,      // command buffer submitted
	Other,
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PASSEND_H_
