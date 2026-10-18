#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUFATAL_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUFATAL_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>

namespace Libs::Graphics {

// Exit statuses for unrecoverable GPU errors (the generic fatal error exits with 65).
inline constexpr int kExitGpuDeviceLost = 66;
inline constexpr int kExitGpuOutOfMemory = 67;

// An unrecoverable Vulkan error: logs what failed and why in plain words (logcat tag KytyGpu,
// the emulator log), shows it in a message box for up to 30 s with --gpu-fatal-dialog, then exits
// with kExitGpuDeviceLost, kExitGpuOutOfMemory or 65. Safe from any thread; when several threads
// hit it (device loss fails every wait at once) the first one reports and the rest block.
[[noreturn]] void GpuFatal(const char* what, vk::Result result);

// Test hook: with KYTY_GPU_FATAL_TEST=<seconds> in the environment (--env), true once that long
// after the first call, so a submit can report VK_ERROR_DEVICE_LOST and the exit path (log,
// message box, exit status) can be checked on a device. False when the variable is unset.
[[nodiscard]] bool GpuFatalTestDue();

// Diagnostics run by GpuFatal after the fatal line is logged and before the exit (e.g. the GPU
// timer's in-flight regions on a device loss). At most a few hooks; each runs once.
using GpuFatalHook = void (*)(vk::Result result);
void AddGpuFatalHook(GpuFatalHook hook);

// VK_EXT_device_fault: after a device loss, logs the faulting GPU addresses the driver reports
// ("[gpu-fault]" lines) before the other hooks run. Called once the device exists with the
// extension and its deviceFault feature enabled.
void EnableDeviceFaultReport(vk::Device device);
// Addresses the device-fault report found (empty before a loss or without the extension), for
// hooks that map them to allocations.
[[nodiscard]] std::span<const uint64_t> ReportedFaultAddresses();

inline void CheckGpuResult(const char* what, vk::Result result) {
	if (result != vk::Result::eSuccess) {
		GpuFatal(what, result);
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUFATAL_H_
