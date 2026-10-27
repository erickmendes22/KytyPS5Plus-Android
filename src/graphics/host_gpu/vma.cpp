#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "graphics/host_gpu/graphicContext.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <fstream>
#include <optional>
#include <string>

namespace Libs::Graphics {

namespace {

struct HeapTotals {
	uint64_t usage  = 0; // driver-reported (VK_EXT_memory_budget) or VMA's own estimate
	uint64_t budget = 0;
	uint64_t blocks = 0; // VMA block bytes
	uint64_t allocs = 0; // VMA allocation bytes
};

// The heaps the collectors count: device-local ones on discrete GPUs, all on unified memory.
HeapTotals SumHeaps(const GraphicContext& ctx) {
	HeapTotals totals {};
	if (ctx.allocator == nullptr) {
		return totals;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(ctx.allocator, budgets);
	const auto& properties = ctx.physical_device_memory_properties;
	const bool  discrete =
	    ctx.physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	for (uint32_t heap = 0; heap < properties.memoryHeapCount; heap++) {
		const bool device_local = static_cast<bool>(properties.memoryHeaps[heap].flags &
		                                            vk::MemoryHeapFlagBits::eDeviceLocal);
		if (discrete && !device_local) {
			continue;
		}
		totals.usage += budgets[heap].usage;
		totals.budget += budgets[heap].budget;
		totals.blocks += budgets[heap].statistics.blockBytes;
		totals.allocs += budgets[heap].statistics.allocationBytes;
	}
	return totals;
}

// MemAvailable from /proc/meminfo (Linux, Android); 0 elsewhere or when unreadable.
std::optional<uint64_t> ReadMemAvailable() {
	std::ifstream meminfo("/proc/meminfo");
	std::string   key;
	uint64_t      kib = 0;
	std::string   unit;
	while (meminfo >> key >> kib >> unit) {
		if (key == "MemAvailable:") {
			return kib * 1024;
		}
	}
	return std::nullopt;
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

} // namespace

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}
	// --vma-block-size: smaller blocks return to the system once eviction empties them.
	info.preferredLargeHeapBlockSize = uint64_t {Config::GetVmaBlockSizeMb()} * GpuBudget::MiB;

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	InitMemoryBudget();
	return true;
}

void GraphicContext::InitMemoryBudget() {
	const auto option = Config::GetGpuMemoryBudgetMb();
	if (option == 0 || allocator == nullptr) {
		Log::WriteToConsoleAndLog(fmt::sprintf(
		    "GPU memory budget: off (legacy heap-size thresholds), vma block=%u MiB\n",
		    Config::GetVmaBlockSizeMb()));
		return;
	}
	const auto totals    = SumHeaps(*this);
	auto       available = ReadMemAvailable().value_or(0);
	if (available == 0 && memory_budget_ext_enabled && totals.budget > totals.usage) {
		// Drivers report usage + 90 % of what the system can spare (Mesa).
		available = (totals.budget - totals.usage) / 9 * 10;
	}
	memory_budget_configured = option > 0 ? uint64_t {static_cast<uint32_t>(option)} * GpuBudget::MiB
	                                      : GpuBudget::AutoBudget(available);
	memory_budget_effective  = memory_budget_configured;
	const auto thresholds    = GpuBudget::ForBudget(memory_budget_configured);
	Log::WriteToConsoleAndLog(fmt::sprintf(
	     "GPU memory budget: %s %" PRIu64 " MiB (MemAvailable %" PRIu64 " MiB, driver usage %" PRIu64
	     " budget %" PRIu64 " MiB) -> gc trigger/pressure/critical %" PRIu64 "/%" PRIu64 "/%" PRIu64
	     " MiB, vma block=%u MiB\n",
	     option > 0 ? "fixed" : "auto", memory_budget_configured / GpuBudget::MiB,
	     available / GpuBudget::MiB, totals.usage / GpuBudget::MiB, totals.budget / GpuBudget::MiB,
	     thresholds.trigger / GpuBudget::MiB, thresholds.pressure / GpuBudget::MiB,
	     thresholds.critical / GpuBudget::MiB, Config::GetVmaBlockSizeMb()));
}

uint64_t GraphicContext::UpdateMemoryBudget() {
	if (!MemoryBudgetEnabled()) {
		return memory_budget_effective;
	}
	constexpr int64_t UpdateIntervalNs = 1'000'000'000;
	const auto        now              = NowNs();
	if (memory_budget_update_ns != 0 && now - memory_budget_update_ns < UpdateIntervalNs) {
		return memory_budget_effective;
	}
	memory_budget_update_ns = now;
	// Makes VMA re-query VK_EXT_memory_budget; it otherwise refreshes only every 30 allocations.
	vmaSetCurrentFrameIndex(allocator, static_cast<uint32_t>(now / UpdateIntervalNs));
	const auto totals   = SumHeaps(*this);
	const auto previous = memory_budget_effective;
	// Only unified-memory devices share this headroom with the CPU. Keep discrete-GPU policy.
	const auto available = physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu
	                           ? std::nullopt : ReadMemAvailable();
	memory_budget_effective =
	    GpuBudget::Effective(memory_budget_configured, totals.usage,
	                         memory_budget_ext_enabled ? totals.budget : 0, available);
	// PPSA24156 was repeatedly rebounding from ~1.7 GiB to 4 GiB while only
	// 0.8-1.1 GiB of shared RAM was available. Do not immediately refill
	// evictable allocations after the collector frees a little headroom.
	// Keep the stable recovery rule limited to this title and shared-memory GPUs.
	std::string title_id;
	if (available &&
	    Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) &&
	    title_id == "PPSA24156") {
		std::optional<uint64_t> driver_headroom;
		if (memory_budget_ext_enabled) {
			driver_headroom = totals.budget > totals.usage
			                      ? totals.budget - totals.usage : 0;
		}
		memory_budget_effective = GpuBudget::StableRecovery(
		    previous, memory_budget_effective, available, driver_headroom);
	}
	// Log real moves only: the headroom jitters by a few MiB every second.
	constexpr uint64_t LogStep = 128 * GpuBudget::MiB;
	const auto         moved   = memory_budget_effective > previous
	                                 ? memory_budget_effective - previous
	                                 : previous - memory_budget_effective;
	if (moved >= LogStep || (moved != 0 && memory_budget_effective == memory_budget_configured)) {
		Log::WriteToConsoleAndLog(fmt::sprintf(
		     "GPU memory budget: %" PRIu64 " -> %" PRIu64 " MiB (driver usage %" PRIu64
		     " budget %" PRIu64 " MiB, MemAvailable %" PRIu64 " MiB, system_sample=%u)\n",
		     previous / GpuBudget::MiB, memory_budget_effective / GpuBudget::MiB,
		     totals.usage / GpuBudget::MiB, totals.budget / GpuBudget::MiB,
		     available.value_or(0) / GpuBudget::MiB, available.has_value() ? 1u : 0u));
	}
	return memory_budget_effective;
}

uint64_t GraphicContext::GetLiveDeviceMemory() const {
	const auto totals = SumHeaps(*this);
	const auto usage  = std::max(totals.usage, totals.blocks);
	return usage - (totals.blocks - std::min(totals.allocs, totals.blocks));
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto        result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &image.allocation, nullptr));
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
