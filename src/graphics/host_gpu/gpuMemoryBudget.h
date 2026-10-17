#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUMEMORYBUDGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUMEMORYBUDGET_H_

#include <algorithm>
#include <cstdint>

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

[[nodiscard]] constexpr Thresholds ForBudget(uint64_t budget) {
	return {budget / 100 * 55, budget / 100 * 80, budget / 100 * 92};
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
                                           uint64_t driver_budget) {
	if (driver_budget == 0 || configured == 0) {
		return configured;
	}
	const auto headroom = driver_budget > usage ? driver_budget - usage : 0;
	if (headroom >= kLowWater) {
		return configured;
	}
	const auto deficit = kLowWater - headroom;
	const auto shrunk  = usage > deficit ? usage - deficit : 0;
	return std::clamp(shrunk, std::min(kRuntimeFloor, configured), configured);
}

} // namespace Libs::Graphics::GpuBudget

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUMEMORYBUDGET_H_
