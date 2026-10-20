#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORTARGETMEMO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORTARGETMEMO_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

// Memo of ResolveRenderColorTarget's register -> RenderColorInfo translation (the description,
// guest mip/layer and export mapping; not the image, which the texture cache still looks up),
// one slot per color target slot (bind-memo bit RenderTargetDesc). The translation is a pure
// function of the slot's decoded CB registers and the draw's slice offset, so a slot hits only
// when both equal the stored ones (HW::RenderTarget's defaulted operator== compares every field
// of every register struct).
class ColorTargetMemo {
public:
	static constexpr uint32_t Slots = 8;

	[[nodiscard]] const RenderColorInfo* Find(uint32_t slot, const HW::RenderTarget& rt,
	                                          uint32_t slice_offset) const {
		if (slot >= Slots) return nullptr;
		const auto& entry = m_slots[slot];
		return entry.valid && entry.slice_offset == slice_offset && entry.rt == rt ? &entry.info : nullptr;
	}
	void Store(uint32_t slot, const HW::RenderTarget& rt, uint32_t slice_offset, const RenderColorInfo& info) {
		if (slot >= Slots) return;
		auto& entry        = m_slots[slot];
		entry.valid        = true;
		entry.rt           = rt;
		entry.slice_offset = slice_offset;
		entry.info         = info;
		entry.info.image_id = {};
	}

private:
	struct Entry {
		bool             valid        = false;
		HW::RenderTarget rt;
		uint32_t         slice_offset = 0;
		RenderColorInfo  info;
	};
	std::array<Entry, Slots> m_slots {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORTARGETMEMO_H_
