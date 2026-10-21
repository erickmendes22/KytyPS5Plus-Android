#include "graphics/host_gpu/renderer/pipeline/bindMemo.h"

#include "graphics/host_gpu/renderer/liveKnobs.h"

#include <atomic>
#include <cstdlib>

namespace Libs::Graphics::BindMemo {

namespace {

// Proven: c15_bind6 / c15_bind7 (gpu-thread:process -1.8..-2.7 ms/frame at the same GPU clock step).
constexpr uint32_t TurnipDefault = TextureDesc | ColorClearScan;
std::atomic<uint32_t> g_device_default {0};

} // namespace

void SetDeviceDefault(bool mesa_turnip) {
	g_device_default.store(mesa_turnip ? TurnipDefault : 0u, std::memory_order_relaxed);
}

uint32_t Bits() {
	static const int64_t env_override = [] {
		const char* value = std::getenv("KYTY_BIND_MEMO");
		return value != nullptr ? static_cast<int64_t>(std::strtoul(value, nullptr, 0)) : int64_t {-1};
	}();
	const int64_t fallback =
	    env_override >= 0 ? env_override : int64_t {g_device_default.load(std::memory_order_relaxed)};
	return static_cast<uint32_t>(LiveKnobs::Int("bind-memo", fallback));
}

} // namespace Libs::Graphics::BindMemo

