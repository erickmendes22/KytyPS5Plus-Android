#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDMEMO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDMEMO_H_

#include <cstdint>

// Binding-preparation memos on the GPU command thread. Each is selected by one bit of the
// bind-memo mask: the live knob bind-memo=<mask> (KYTY_LIVE_KNOBS=1) when present, otherwise
// KYTY_BIND_MEMO=<mask>, otherwise the default: TextureDesc | ColorClearScan with Mesa Turnip
// (SetDeviceDefault), 0 elsewhere. KYTY_BIND_MEMO=0 opts out.
namespace Libs::Graphics::BindMemo {

enum Bit : uint32_t {
	// ResolveTexture's descriptor -> ImageDesc translation, per program image slot (TextureDescMemo).
	TextureDesc = 1u << 0u,
	// ResolveRenderColorTarget's register -> description translation, per slot (ColorTargetMemo).
	RenderTargetDesc = 1u << 1u,
	// TextureCache::MaterializeColorClear: word-wise uniformity test of color metadata slices.
	ColorClearScan = 1u << 3u,
};

// The mask without KYTY_BIND_MEMO or the live knob; set once by the renderer at startup.
void SetDeviceDefault(bool mesa_turnip);
[[nodiscard]] uint32_t Bits();

} // namespace Libs::Graphics::BindMemo

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDMEMO_H_
