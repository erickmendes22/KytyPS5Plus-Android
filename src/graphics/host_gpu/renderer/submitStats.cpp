#include "graphics/host_gpu/renderer/submitStats.h"

#include "graphics/host_gpu/renderer/liveKnobs.h"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Libs::Graphics::SubmitStats {

namespace {

constexpr size_t kCallerSlots = 32;
std::array<std::atomic<uintptr_t>, kCallerSlots> g_callers {};
std::array<std::atomic<uint64_t>, kCallerSlots>  g_caller_counts {};

// Work buckets: 0, 1-15, 16-63, 64-255, 256+.
constexpr size_t kBuckets = 5;
constexpr const char* kBucketNames[kBuckets] = {"w0", "w1-15", "w16-63", "w64-255", "w256+"};
std::array<std::atomic<uint64_t>, kBuckets> g_bucket_count {};
std::array<std::atomic<uint64_t>, kBuckets> g_bucket_ns {};

size_t Bucket(uint32_t work) {
	if (work == 0) return 0;
	if (work < 16) return 1;
	if (work < 64) return 2;
	if (work < 256) return 3;
	return 4;
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SUBMIT_STATS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void NoteCaller(const void* return_address) {
	if (!Enabled()) return;
	const auto key  = reinterpret_cast<uintptr_t>(return_address);
	auto       slot = (key >> 2u) % kCallerSlots;
	for (size_t i = 0; i < kCallerSlots; i++, slot = (slot + 1) % kCallerSlots) {
		auto current = g_callers[slot].load(std::memory_order_relaxed);
		if (current == 0) {
			uintptr_t expected = 0;
			g_callers[slot].compare_exchange_strong(expected, key, std::memory_order_relaxed);
			current = g_callers[slot].load(std::memory_order_relaxed);
		}
		if (current == key) {
			g_caller_counts[slot].fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
}

void NoteSubmit(uint32_t work, uint64_t submit_ns) {
	if (!Enabled()) return;
	const auto b = Bucket(work);
	g_bucket_count[b].fetch_add(1, std::memory_order_relaxed);
	g_bucket_ns[b].fetch_add(submit_ns, std::memory_order_relaxed);
}

void Append(std::string& out, double per_frame) {
	if (!Enabled()) return;
	char part[96];
	out += " submits:";
	for (size_t b = 0; b < kBuckets; b++) {
		const auto n  = g_bucket_count[b].exchange(0, std::memory_order_relaxed);
		const auto ns = g_bucket_ns[b].exchange(0, std::memory_order_relaxed);
		if (n == 0) continue;
		std::snprintf(part, sizeof(part), " %s=%.1f(%.2fms each)", kBucketNames[b],
		              static_cast<double>(n) * per_frame, static_cast<double>(ns) / 1e6 / static_cast<double>(n));
		out += part;
	}
	std::vector<std::pair<uint64_t, uintptr_t>> callers;
	for (size_t i = 0; i < kCallerSlots; i++) {
		const auto n = g_caller_counts[i].exchange(0, std::memory_order_relaxed);
		if (n != 0) callers.emplace_back(n, g_callers[i].load(std::memory_order_relaxed));
	}
	std::sort(callers.rbegin(), callers.rend());
	out += " from:";
	for (size_t i = 0; i < std::min<size_t>(callers.size(), 6); i++) {
		Dl_info info {};
		uintptr_t offset = callers[i].second;
		if (dladdr(reinterpret_cast<const void*>(callers[i].second), &info) != 0 && info.dli_fbase != nullptr) {
			offset -= reinterpret_cast<uintptr_t>(info.dli_fbase);
		}
		std::snprintf(part, sizeof(part), " 0x%zx=%.1f", static_cast<size_t>(offset),
		              static_cast<double>(callers[i].first) * per_frame);
		out += part;
	}
}

uint32_t IdleMinWork() {
	const auto v = LiveKnobs::Int("idle_min", 16);
	return v < 0 ? 16u : static_cast<uint32_t>(std::min<int64_t>(v, 1u << 30));
}

uint32_t CoalesceMax() {
	const auto v = LiveKnobs::Int("coalesce_max", 8);
	return v < 0 ? 8u : static_cast<uint32_t>(std::min<int64_t>(v, 1024));
}

int64_t InterruptFlushNs(int64_t fallback) {
	const auto v = LiveKnobs::Int("irq_flush_ms", -1);
	return v < 0 ? fallback : v * 1'000'000;
}

} // namespace Libs::Graphics::SubmitStats
