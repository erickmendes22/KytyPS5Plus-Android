#include "graphics/host_gpu/renderer/rpStats.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::RpStats {

namespace {
std::atomic<uint64_t> g_clears {0};
std::atomic<uint64_t> g_full_color {0};
std::atomic<uint64_t> g_absorbable {0};
std::atomic<uint64_t> g_partial {0};
std::atomic<uint64_t> g_bytes {0};
} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_RP_STATS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void OnClear(bool full_color) {
	g_clears.fetch_add(1, std::memory_order_relaxed);
	if (full_color) g_full_color.fetch_add(1, std::memory_order_relaxed);
}

void OnAttachment(bool full_area, uint64_t bytes) {
	if (full_area) {
		g_absorbable.fetch_add(1, std::memory_order_relaxed);
		g_bytes.fetch_add(bytes, std::memory_order_relaxed);
	} else {
		g_partial.fetch_add(1, std::memory_order_relaxed);
	}
}

void Append(std::string& out, double per_frame) {
	if (!Enabled()) return;
	const auto take = [](std::atomic<uint64_t>& c) { return static_cast<double>(c.exchange(0, std::memory_order_relaxed)); };
	char part[200];
	std::snprintf(part, sizeof(part), " rp_clears: all=%.1f full_color=%.1f absorbable=%.1f (%.1f MB) partial_pass=%.1f",
	              take(g_clears) * per_frame, take(g_full_color) * per_frame, take(g_absorbable) * per_frame,
	              take(g_bytes) / 1e6 * per_frame, take(g_partial) * per_frame);
	out += part;
}

} // namespace Libs::Graphics::RpStats
