#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUMEMORYBUDGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUMEMORYBUDGET_H_

#include <algorithm>
#include <cstdint>
#include <optional>

// --gpu-memory-budget: one device-memory budget for unified-memory GPUs (Android), from which the
// texture and buffer cache collectors take their thresholds. The legacy thresholds derive from the
// heap size the driver reports, which on a 16 GB tablet is the whole shared RAM: eviction under
// pressure then never starts before the system kills the app. Pure functions, unit-tested.
namespace Libs::Graphics::GpuBudget {

inline constexpr uint64_t MiB = uint64_t {1} << 20u;
inline constexpr uint64_t GiB = uint64_t {1} << 30u;

// Auto budget: what the system can spare at start, less what the emulator itself needs on the
// CPU side (guest memory, JIT, shader caches: ~4 GB of PSS in Astro Bot). The floor keeps the
// budget above Astro Bot's gameplay working set (~3.8-4.0 GB of live allocations, 1.6 GB of it
// fixed buffers): a 2 GiB budget kept the collector critical from boot and cost ~40 % fps.
inline constexpr uint64_t kAutoCap        = 4608 * MiB;
inline constexpr uint64_t kAutoCpuReserve = 4 * GiB;
inline constexpr uint64_t kAutoFloor      = 3584 * MiB;
// At runtime the budget shrinks when the driver's headroom (Turnip: 90 % of MemAvailable) falls
// below this, so the collector gives memory back before the low-memory killer acts. While Astro
// Bot runs on the 16 GB tablet MemAvailable sits around 1.7 GB, so this must stay well below that.
inline constexpr uint64_t kLowWater     = 768 * MiB;
inline constexpr uint64_t kRuntimeFloor = 1536 * MiB;

struct Thresholds {
	uint64_t trigger  = 0; // evict images unused for a while, no downloads
	uint64_t pressure = 0; // evict sooner (--gpu-evict-written: also stale GPU-written images)
	uint64_t critical = 0; // evict aggressively
};

// A GC pass may write back at most one cold GPU-written image. Under critical pressure
// allow a larger image only if it fits the already allocated reusable staging buffer.
[[nodiscard]] constexpr uint64_t WrittenDownloadLimit(bool critical, uint64_t capacity) {
	return std::min(capacity, (critical ? 64u : 32u) * MiB);
}

[[nodiscard]] constexpr uint64_t WrittenIdleFrames(bool critical) {
	return critical ? 8 : 32;
}

// Window size bounds both the download ring and the linear tiler scratch.
inline constexpr uint64_t        kWrittenChunkBytes = 8 * MiB;
[[nodiscard]] constexpr uint32_t ReadbackRows(uint32_t remaining, uint32_t alignment,
                                              uint64_t guest_row, uint64_t linear_row,
                                              uint64_t capacity) {
	if (!remaining || !alignment || !guest_row || !linear_row) return 0;
	const auto available = std::min(capacity / guest_row, capacity / linear_row);
	const auto blocks    = available / alignment;
	if (!blocks) return 0;
	return static_cast<uint32_t>(std::min<uint64_t>(remaining, blocks * alignment));
}

[[nodiscard]] constexpr Thresholds ForBudget(uint64_t budget) {
	return {budget / 100 * 55, budget / 100 * 80, budget / 100 * 92};
}

// PPSA24156: the two Android REASON_LOW_MEMORY sessions (09:54 / 09:58)
// exhausted shared RAM while creating hundreds of sampled images. Vulkan
// vmaCreateImage had not yet returned OOM, so the existing allocation-failure
// recovery was never attempted. Try that *same* safe eviction ahead of the next
// image allocation when the OS is running short of memory. Throttle attempts
// by allocation growth / frame age; this does not modify GPU budget thresholds.
inline constexpr uint64_t kGtPreallocLowRam = 1536 * MiB;
inline constexpr uint64_t kGtPreallocMinUsage = 1536 * MiB;
inline constexpr uint64_t kGtPreallocGrowth = 64 * MiB;

[[nodiscard]] constexpr bool ShouldPreemptGtImageAllocation(
    std::optional<uint64_t> available, uint64_t device_usage,
    uint64_t previous_attempt_usage, uint64_t frames_since_attempt) {
	if (!available || *available >= kGtPreallocLowRam ||
	    device_usage < kGtPreallocMinUsage) return false;
	if (previous_attempt_usage == 0) return true;
	// Re-check when GPU residency grows significantly within a single frame,
	// or when a new frame has made more old cache entries safe to reclaim.
	return (device_usage > previous_attempt_usage &&
	        device_usage - previous_attempt_usage >= kGtPreallocGrowth) ||
	       frames_since_attempt >= 3;
}

// mem_available: MemAvailable at start (bytes); 0 when unknown.
[[nodiscard]] constexpr uint64_t AutoBudget(uint64_t mem_available) {
	if (mem_available == 0) {
		return kAutoCap;
	}
	const auto spare = mem_available > kAutoCpuReserve ? mem_available - kAutoCpuReserve : 0;
	return std::clamp(spare, kAutoFloor, kAutoCap);
}

// configured: the budget chosen at start. usage/driver_budget: VK_EXT_memory_budget heap usage and
// budget (budget = usage + what the system could still give). Returns the budget to use now.
[[nodiscard]] constexpr uint64_t Effective(uint64_t configured, uint64_t usage,
                                           uint64_t driver_budget,
                                           std::optional<uint64_t> system_available = std::nullopt) {
	if (configured == 0 || (driver_budget == 0 && !system_available)) {
		return configured;
	}
	// Unified-memory GPUs compete with guest/JIT allocations too. Driver reporting can be
	// unavailable or lag behind CPU-side growth, so use the smaller observed headroom.
	auto headroom = driver_budget == 0 ? UINT64_MAX
	                : driver_budget > usage ? driver_budget - usage : 0;
	if (system_available) {
		headroom = std::min(headroom, *system_available);
	}
	if (headroom >= kLowWater) {
		return configured;
	}
	const auto deficit = kLowWater - headroom;
	const auto shrunk  = usage > deficit ? usage - deficit : 0;
	return std::clamp(shrunk, std::min(kRuntimeFloor, configured), configured);
}

// PPSA24156 replay (PID 1517, 2026-10-29): Effective() followed growing
// usage back up to 3584 MiB while available RAM stayed below 200 MiB.
// Keep a pressure reduction latched until headroom has actually recovered;
// then restore at most 128 MiB per one-second sample, not in a single jump.
[[nodiscard]] constexpr uint64_t StabilizeGtBudget(
    uint64_t configured, uint64_t previous, uint64_t proposed,
    uint64_t usage, uint64_t driver_budget,
    std::optional<uint64_t> available) {
    if (configured == 0) return 0;
    if (previous == 0) previous = configured;
    auto headroom = driver_budget == 0 ? UINT64_MAX
        : driver_budget > usage ? driver_budget - usage : 0;
    if (available) headroom = std::min(headroom, *available);
    if (headroom < 2 * GiB) return std::min(previous, proposed);
    const auto step = std::min(configured - std::min(previous, configured), 128 * MiB);
    return std::min(proposed, previous + step);
}

} // namespace Libs::Graphics::GpuBudget

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUMEMORYBUDGET_H_
