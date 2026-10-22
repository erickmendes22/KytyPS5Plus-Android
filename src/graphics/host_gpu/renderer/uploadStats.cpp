#include "graphics/host_gpu/renderer/uploadStats.h"

#include <xxhash.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Libs::Graphics::UploadStats {

namespace {

// Size buckets: <=256 B, <=1 KiB, <=4 KiB, <=16 KiB, larger.
constexpr size_t      kBuckets                = 5;
constexpr const char* kBucketNames[kBuckets] = {"256", "1K", "4K", "16K", "big"};

std::atomic<uint64_t>                       g_calls {0};
std::atomic<uint64_t>                       g_bytes {0};
std::atomic<uint64_t>                       g_ns {0};
std::atomic<uint64_t>                       g_same_calls {0};
std::atomic<uint64_t>                       g_same_bytes {0};
std::atomic<uint64_t>                       g_sync_calls {0};
std::atomic<uint64_t>                       g_sync_bytes {0};
std::array<std::atomic<uint64_t>, kBuckets> g_bucket {};

std::mutex                                    g_hash_mutex;
std::unordered_map<uint64_t, uint64_t>        g_last_hash; // (vaddr ^ size << 48) -> content hash
std::atomic<uint64_t>                         g_unique {0};
std::atomic<uint64_t>                         g_promotions {0};

size_t Bucket(uint64_t size) {
	if (size <= 256) return 0;
	if (size <= 1024) return 1;
	if (size <= 4096) return 2;
	if (size <= 16384) return 3;
	return 4;
}

uint64_t Take(std::atomic<uint64_t>& value) {
	return value.exchange(0, std::memory_order_relaxed);
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_UPLOAD_STATS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void NoteStream(uint64_t vaddr, uint64_t size, const void* data, uint64_t copy_ns) {
	if (!Enabled()) return;
	g_calls.fetch_add(1, std::memory_order_relaxed);
	g_bytes.fetch_add(size, std::memory_order_relaxed);
	g_ns.fetch_add(copy_ns, std::memory_order_relaxed);
	g_bucket[Bucket(size)].fetch_add(1, std::memory_order_relaxed);
	const uint64_t hash = XXH3_64bits(data, static_cast<size_t>(size));
	const uint64_t key  = vaddr ^ (size << 48u);
	std::lock_guard lock(g_hash_mutex);
	auto [it, inserted] = g_last_hash.try_emplace(key, hash);
	if (inserted) {
		g_unique.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (it->second == hash) {
		g_same_calls.fetch_add(1, std::memory_order_relaxed);
		g_same_bytes.fetch_add(size, std::memory_order_relaxed);
	}
	it->second = hash;
}

void NoteSync(uint64_t bytes) {
	if (!Enabled()) return;
	g_sync_calls.fetch_add(1, std::memory_order_relaxed);
	g_sync_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NotePromotion() {
	g_promotions.fetch_add(1, std::memory_order_relaxed);
}

void Append(std::string& out, double per_frame) {
	char part[512];
	if (const auto promoted = Take(g_promotions); promoted != 0) {
		std::snprintf(part, sizeof(part), " stream_promoted=%.1f", static_cast<double>(promoted) * per_frame);
		out += part;
	}
	if (!Enabled()) return;
	std::snprintf(part, sizeof(part),
	              " stream: calls=%.0f KB=%.0f copy=%.2fms unchanged: calls=%.0f KB=%.0f new_ranges=%.0f sizes:",
	              static_cast<double>(Take(g_calls)) * per_frame,
	              static_cast<double>(Take(g_bytes)) / 1024.0 * per_frame,
	              static_cast<double>(Take(g_ns)) / 1e6 * per_frame,
	              static_cast<double>(Take(g_same_calls)) * per_frame,
	              static_cast<double>(Take(g_same_bytes)) / 1024.0 * per_frame,
	              static_cast<double>(Take(g_unique)) * per_frame);
	out += part;
	for (size_t i = 0; i < kBuckets; i++) {
		std::snprintf(part, sizeof(part), " %s=%.0f", kBucketNames[i], static_cast<double>(Take(g_bucket[i])) * per_frame);
		out += part;
	}
	std::snprintf(part, sizeof(part), " sync: calls=%.0f KB=%.0f",
	              static_cast<double>(Take(g_sync_calls)) * per_frame,
	              static_cast<double>(Take(g_sync_bytes)) / 1024.0 * per_frame);
	out += part;
}

} // namespace Libs::Graphics::UploadStats
