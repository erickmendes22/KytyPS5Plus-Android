#include "graphics/host_gpu/gpuFatal.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cinttypes>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Libs::Graphics {

namespace {

struct Reason {
	const char* title;
	const char* text;
	int         status;
};

Reason Classify(vk::Result result) {
	switch (result) {
		case vk::Result::eErrorDeviceLost:
			return {"GPU device lost",
			        "The GPU driver reset the device (VK_ERROR_DEVICE_LOST), usually after a GPU hang "
			        "or fault. The game cannot continue; restart it. If it recurs at the same place, "
			        "try another GPU driver.",
			        kExitGpuDeviceLost};
		case vk::Result::eErrorOutOfDeviceMemory:
		case vk::Result::eErrorOutOfHostMemory:
		case vk::Result::eErrorOutOfPoolMemory:
		case vk::Result::eErrorFragmentedPool:
			return {"Out of GPU memory",
			        "The GPU ran out of memory even after evicting cached textures. Close other apps "
			        "and restart the game, or lower --gpu-memory-budget / --render-scale.",
			        kExitGpuOutOfMemory};
		default:
			return {"GPU error", "An unexpected Vulkan error stopped the emulator.", 65};
	}
}

// Shows the message box on a helper thread so a dialog nobody dismisses cannot hold the exit
// forever (SDL's Android message box blocks until it is answered).
void ShowDialog(const char* title, const std::string& message) {
	constexpr auto Timeout = std::chrono::seconds(30);
	auto           done    = std::make_shared<std::binary_semaphore>(0);
	std::thread([done, title, message] {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message.c_str(), nullptr);
		done->release();
	}).detach();
	(void)done->try_acquire_for(Timeout);
}

constexpr size_t                       kMaxHooks = 4;
std::array<std::atomic<GpuFatalHook>, kMaxHooks> g_hooks {};

void RunHooks(vk::Result result) {
	for (auto& slot: g_hooks) {
		if (const auto hook = slot.load(std::memory_order_acquire); hook != nullptr) {
			hook(result);
		}
	}
}

vk::Device                g_fault_device = nullptr;
std::array<uint64_t, 16>  g_fault_addresses {};
std::atomic<size_t>       g_fault_address_count {0};

const char* FaultAddressTypeName(vk::DeviceFaultAddressTypeEXT type) {
	switch (type) {
		case vk::DeviceFaultAddressTypeEXT::eNone: return "none";
		case vk::DeviceFaultAddressTypeEXT::eReadInvalid: return "read-invalid";
		case vk::DeviceFaultAddressTypeEXT::eWriteInvalid: return "write-invalid";
		case vk::DeviceFaultAddressTypeEXT::eExecuteInvalid: return "execute-invalid";
		case vk::DeviceFaultAddressTypeEXT::eInstructionPointerUnknown: return "ip-unknown";
		case vk::DeviceFaultAddressTypeEXT::eInstructionPointerInvalid: return "ip-invalid";
		case vk::DeviceFaultAddressTypeEXT::eInstructionPointerFault: return "ip-fault";
		default: return "?";
	}
}

void ReportDeviceFault(vk::Result result) {
	if (result != vk::Result::eErrorDeviceLost || g_fault_device == nullptr ||
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT == nullptr) {
		return;
	}
	const auto device = static_cast<VkDevice>(g_fault_device);
	VkDeviceFaultCountsEXT counts {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
	auto status = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT(device, &counts, nullptr);
	LOGF("[gpu-fault] vkGetDeviceFaultInfoEXT counts: %s addresses=%u vendor=%u binary=%" PRIu64 "\n",
	     vk::to_string(static_cast<vk::Result>(status)).c_str(), counts.addressInfoCount,
	     counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	if (status != VK_SUCCESS && status != VK_INCOMPLETE) {
		return;
	}
	std::array<VkDeviceFaultAddressInfoEXT, 16> addresses {};
	std::array<VkDeviceFaultVendorInfoEXT, 16>  vendors {};
	counts.addressInfoCount = std::min<uint32_t>(counts.addressInfoCount, addresses.size());
	counts.vendorInfoCount  = std::min<uint32_t>(counts.vendorInfoCount, vendors.size());
	counts.vendorBinarySize = 0;
	VkDeviceFaultInfoEXT info {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	status = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT(device, &counts, &info);
	LOGF("[gpu-fault] %s: %s\n", vk::to_string(static_cast<vk::Result>(status)).c_str(),
	     info.description);
	size_t stored = 0;
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& address = addresses[i];
		LOGF("[gpu-fault] address %u: %s 0x%016" PRIx64 " precision 0x%" PRIx64 "\n", i,
		     FaultAddressTypeName(static_cast<vk::DeviceFaultAddressTypeEXT>(address.addressType)),
		     static_cast<uint64_t>(address.reportedAddress),
		     static_cast<uint64_t>(address.addressPrecision));
		if (stored < g_fault_addresses.size()) {
			g_fault_addresses[stored++] = address.reportedAddress;
		}
	}
	g_fault_address_count.store(stored, std::memory_order_release);
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		LOGF("[gpu-fault] vendor %u: %s code=0x%" PRIx64 " data=0x%" PRIx64 "\n", i,
		     vendors[i].description, static_cast<uint64_t>(vendors[i].vendorFaultCode),
		     static_cast<uint64_t>(vendors[i].vendorFaultData));
	}
}

} // namespace

void EnableDeviceFaultReport(vk::Device device) {
	g_fault_device = device;
	AddGpuFatalHook(ReportDeviceFault);
}

std::span<const uint64_t> ReportedFaultAddresses() {
	return {g_fault_addresses.data(), g_fault_address_count.load(std::memory_order_acquire)};
}

void AddGpuFatalHook(GpuFatalHook hook) {
	for (auto& slot: g_hooks) {
		GpuFatalHook expected = nullptr;
		if (slot.load(std::memory_order_acquire) == hook ||
		    slot.compare_exchange_strong(expected, hook, std::memory_order_acq_rel)) {
			return;
		}
	}
}

bool GpuFatalTestDue() {
	using Clock               = std::chrono::steady_clock;
	static const auto s_delay = [] {
		const char* value = std::getenv("KYTY_GPU_FATAL_TEST");
		return value == nullptr ? -1 : std::atoi(value);
	}();
	static const auto s_start = Clock::now();
	return s_delay >= 0 && Clock::now() - s_start >= std::chrono::seconds(s_delay);
}

void GpuFatal(const char* what, vk::Result result) {
	static std::atomic_flag s_reported;
	if (s_reported.test_and_set()) {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::hours(1));
		}
	}
	const auto  reason  = Classify(result);
	const auto  message = fmt::format("{}\n\n{} failed: {} ({}).", reason.text, what,
	                                  vk::to_string(result), static_cast<int>(result));
	const auto  line    = fmt::format("GPU fatal: {}: {} failed: {} ({}), exit {}", reason.title, what,
	                                  vk::to_string(result), static_cast<int>(result), reason.status);
	LOGF("%s\n", line.c_str());
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
#if defined(__ANDROID__)
	__android_log_write(ANDROID_LOG_ERROR, "KytyGpu", line.c_str());
#endif
	RunHooks(result);
	Common::DbgExitHandler(__FILE__, __LINE__, message);
	if (Config::GpuFatalDialogEnabled()) {
		ShowDialog(reason.title, message);
	}
	Common::DbgExit(reason.status);
	for (;;) {
		std::this_thread::sleep_for(std::chrono::hours(1));
	}
}

} // namespace Libs::Graphics
