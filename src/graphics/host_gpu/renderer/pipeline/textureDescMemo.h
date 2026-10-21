#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_TEXTUREDESCMEMO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_TEXTUREDESCMEMO_H_

#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// What ResolveTexture derives from an image resource and its descriptor before the texture cache
// lookup.
struct TextureDescEntry {
	TextureCache::ImageDesc desc;
	bool                    shader_conversion = false;
	vk::Format              pixel_format      = vk::Format::eUndefined;
	vk::Format              view_format       = vk::Format::eUndefined;
};

// Memo of that translation, one slot per (program, image index): the last descriptor seen there
// and what it translated to. The translation is a pure function of (ImageResource,
// DescriptorValue), so a slot hits only when both equal the stored ones in full (their defaulted
// operator==, every field). The program pointer only picks the slot list; a reused address is
// still checked field by field. Bounded: the program table is cleared at MaxPrograms.
class TextureDescMemo {
public:
	static constexpr size_t MaxPrograms = 4096;

	struct Slot {
		bool                                  valid = false;
		ShaderRecompiler::IR::DescriptorValue value;
		ShaderRecompiler::IR::ImageResource   resource;
		TextureDescEntry                      entry;
	};

	// The slots of one program (resized to `count`). Valid until the next SlotsFor or Clear.
	[[nodiscard]] std::vector<Slot>& SlotsFor(const void* program, size_t count);

	[[nodiscard]] static const TextureDescEntry* Find(const Slot&                                  slot,
	                                                  const ShaderRecompiler::IR::ImageResource&   resource,
	                                                  const ShaderRecompiler::IR::DescriptorValue& value) {
		return slot.valid && slot.value == value && slot.resource == resource ? &slot.entry : nullptr;
	}
	static const TextureDescEntry& Store(Slot& slot, const ShaderRecompiler::IR::ImageResource& resource,
	                                     const ShaderRecompiler::IR::DescriptorValue& value,
	                                     TextureDescEntry                             entry) {
		slot.valid    = true;
		slot.value    = value;
		slot.resource = resource;
		slot.entry    = std::move(entry);
		return slot.entry;
	}

	void Clear() { m_programs.clear(); }
	[[nodiscard]] size_t Programs() const { return m_programs.size(); }

private:
	std::unordered_map<const void*, std::vector<Slot>> m_programs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_TEXTUREDESCMEMO_H_
