#include "graphics/host_gpu/renderer/pipeline/textureDescMemo.h"

namespace Libs::Graphics {

std::vector<TextureDescMemo::Slot>& TextureDescMemo::SlotsFor(const void* program, size_t count) {
	auto found = m_programs.find(program);
	if (found == m_programs.end()) {
		if (m_programs.size() >= MaxPrograms) {
			m_programs.clear();
		}
		found = m_programs.emplace(program, std::vector<Slot>()).first;
	}
	if (found->second.size() != count) {
		found->second.resize(count);
	}
	return found->second;
}

} // namespace Libs::Graphics
