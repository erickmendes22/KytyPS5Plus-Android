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
// as Dolphin does through libadrenotools. Android/Adreno only; elsewhere it logs and does nothing.
void EnableGpuTurbo();

} // namespace Libs::Graphics::VulkanDriver

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVER_H_
