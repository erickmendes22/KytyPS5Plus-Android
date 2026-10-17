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
void RequestGpuTurbo();
[[nodiscard]] bool GpuTurboRequested();
// Once, after the command line is parsed: turbo on (and the exit hook) or an explicit reset.
void ApplyGpuClockPolicy();
// Idempotent and async-signal-safe.
void SuspendGpuTurbo() noexcept;
void ResumeGpuTurbo() noexcept;
void ReleaseGpuTurbo() noexcept;

} // namespace Libs::Graphics::VulkanDriver

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVER_H_
