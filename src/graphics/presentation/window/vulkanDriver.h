#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVER_H_

// Command-line options for the host Vulkan driver. Kept free of Vulkan headers so main.cpp can
// set them cheaply; the loader side lives in vulkanDriverLoader.h.

#include <string>

namespace Libs::Graphics::VulkanDriver {

// --gpu-driver <dir>: a directory holding an imported driver package (meta.json plus the driver
// .so, the AdrenoTools format used by Eden/Yuzu/Skyline), e.g. Mesa Turnip. Android only; other
// platforms log and ignore it. An empty directory selects the system driver.
void SetCustomDriverDirectory(std::string directory);

// --print-vulkan-caps: dump the selected device's driver, extensions and key features.
void SetPrintCaps(bool enabled);

// --gpu-turbo: ask KGSL to keep the Adreno GPU at its maximum clock (thermal limits still apply),
// as Dolphin does through libadrenotools. The request is device-global and survives the process,
// so it is dropped on every exit path the process sees (Common::AddProcessExitHook), while the
// app is in the background (Suspend/Resume from the SDL lifecycle watch), and at the start of
// every run without --gpu-turbo (a SIGKILL cannot be caught). Android/Adreno only.
// KGSL holds a turbo request as a fixed power level with the GPU governor suspended, so a thermal
// or PM QoS cap clamps that level and nothing raises it again once the cap lifts: the clock stays
// low for the rest of the session. While in the foreground the request is therefore re-taken
// every KYTY_GPU_TURBO_REASSERT_MS (default 2000, 0 = never), which restores the highest level
// the current limits allow.
void RequestGpuTurbo();
[[nodiscard]] bool GpuTurboRequested();
// Once, after the command line is parsed: turbo on (and the exit hook) or an explicit reset.
void ApplyGpuClockPolicy();
// Idempotent. Suspend/Resume run on the lifecycle watch; Release is async-signal-safe.
void SuspendGpuTurbo() noexcept;
void ResumeGpuTurbo() noexcept;
void ReleaseGpuTurbo() noexcept;

} // namespace Libs::Graphics::VulkanDriver

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVER_H_
