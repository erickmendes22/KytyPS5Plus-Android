#include "graphics/host_gpu/renderer/pipeline/bindMemo.h"

#include "graphics/host_gpu/renderer/liveKnobs.h"

#include <cstdlib>

namespace Libs::Graphics::BindMemo {

uint32_t Bits() {
	static const int64_t env_default = [] {
		const char* value = std::getenv("KYTY_BIND_MEMO");
		return value != nullptr ? static_cast<int64_t>(std::strtoul(value, nullptr, 0)) : int64_t {0};
	}();
	return static_cast<uint32_t>(LiveKnobs::Int("bind-memo", env_default));
}

} // namespace Libs::Graphics::BindMemo

