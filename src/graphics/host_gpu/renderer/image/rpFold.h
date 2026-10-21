#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_RPFOLD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_RPFOLD_H_

#include <cstdint>

// KYTY_RP_OPS clear fold (default on with Mesa Turnip; KYTY_RP_OPS=0 opts out, live key rp_ops=0/1
// overrides): a guest full colour clear (TextureCache::ClearImage) is not recorded at once; the
// image keeps it pending. The next render pass that binds the image as a colour attachment over
// its whole area in the image's own format starts with loadOp CLEAR and that value. Any other use
// first records the clear, in the order the guest issued it (Image::GetBarriers: sampling,
// storage, copies, uploads, downloads, a partial-area pass, a view in another format).
// Pure rules shared by Image / TextureCache / RenderExecutor and tests/RpOpsFoldTests.cpp.
namespace Libs::Graphics::RpFold {

// The cleared subresources of the image.
struct Shape {
	bool     color_aspect = false;
	bool     full_image   = false; // every level and layer of the colour aspect
	uint32_t levels       = 1;
	uint32_t layers       = 1;
	uint32_t samples      = 1;
	bool     volume       = false;
};

// Only a single-level, single-layer, single-sample 2D colour image cleared in full can be folded:
// a pass covers exactly one level and layer.
constexpr bool Eligible(const Shape& shape) {
	return shape.color_aspect && shape.full_image && shape.levels == 1 && shape.layers == 1 &&
	       shape.samples == 1 && !shape.volume;
}

// The pass that binds the image as a colour attachment.
struct PassUse {
	uint32_t pass_width      = 0; // render area (host size)
	uint32_t pass_height     = 0;
	uint32_t pass_layers     = 1;
	uint32_t view_base_level = 0;
	uint32_t image_width     = 0; // the image's backing extent
	uint32_t image_height    = 0;
	bool     same_format     = false; // attachment view format == the image's format
};

// loadOp CLEAR over the render area equals the transfer clear only for the whole image in its
// own format (the clear value is interpreted in the view's format).
constexpr bool Absorbs(const PassUse& use) {
	return use.pass_width == use.image_width && use.pass_height == use.image_height &&
	       use.pass_layers == 1 && use.view_base_level == 0 && use.same_format;
}

template <typename Value>
class Tracker {
public:
	void Fold(const Value& value) {
		m_pending = true;
		m_value   = value;
	}
	[[nodiscard]] bool Pending() const { return m_pending; }
	// A layout change of the image for any use. `acquiring`: the colour-attachment acquisition of
	// the pass that may absorb the clear. True: the caller records the clear now (TakeForFlush).
	[[nodiscard]] bool NeedsFlush(bool acquiring) const { return m_pending && !acquiring; }
	Value TakeForFlush() {
		m_pending = false;
		return m_value;
	}
	// At pass begin. True: the pass starts with loadOp CLEAR and `value`. False with Pending():
	// the pass cannot absorb it and the caller records the clear before the pass.
	bool TryAbsorb(const PassUse& use, Value& value) {
		if (!m_pending || !Absorbs(use)) {
			return false;
		}
		m_pending = false;
		value     = m_value;
		return true;
	}

private:
	bool  m_pending = false;
	Value m_value {};
};

} // namespace Libs::Graphics::RpFold

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_RPFOLD_H_ */
