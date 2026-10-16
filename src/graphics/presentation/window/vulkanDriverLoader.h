#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVERLOADER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVERLOADER_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <SDL3/SDL.h>

namespace Libs::Graphics::VulkanDriver {

// Entry point used to create the Vulkan instance. With --gpu-driver on Android this is the
// custom driver opened through libadrenotools; when none is selected, or it fails to load, it is
// SDL's system loader (with a log line saying why). Returns nullptr only if no Vulkan exists.
PFN_vkGetInstanceProcAddr LoadInstanceProcAddr();

// Drops a custom driver whose instance could not be created and returns the system loader's
// entry point instead. Returns nullptr if no custom driver was active.
PFN_vkGetInstanceProcAddr FallBackToSystemDriver();

// True while the instance comes from a custom driver.
bool CustomDriverActive();

// --print-vulkan-caps was given.
bool PrintCapsEnabled();

// Creates the window surface through the loader LoadInstanceProcAddr picked. SDL's own helper
// always calls the system loader, which cannot take an instance created by a custom driver.
bool CreateSurface(SDL_Window* window, vk::Instance instance, vk::SurfaceKHR::CType* out_surface);

// One line naming the driver in use; with --print-vulkan-caps also every device extension and the
// features that decide shader paths (barycentrics, mesh shaders, FP64, 64-bit LDS atomics, ...).
void LogDeviceCaps(vk::PhysicalDevice device);

} // namespace Libs::Graphics::VulkanDriver

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDRIVERLOADER_H_
