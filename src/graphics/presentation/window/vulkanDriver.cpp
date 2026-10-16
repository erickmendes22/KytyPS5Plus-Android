#include "graphics/presentation/window/vulkanDriver.h"

#include "common/logging/log.h"
#include "graphics/presentation/window/vulkanDriverLoader.h"

#include <SDL3/SDL_vulkan.h>

#include <fmt/format.h>
#include <string>
#include <utility>

#if defined(KYTY_ANDROID)
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan_android.h>

#include <filesystem>
#include <fstream>
#include <optional>
#endif

namespace Libs::Graphics::VulkanDriver {

namespace {

struct DriverState {
	std::string               custom_directory;
	bool                      print_caps    = false;
	bool                      custom_active = false;
	PFN_vkGetInstanceProcAddr custom_gipa   = nullptr;
};

DriverState& State() {
	static DriverState state;
	return state;
}

void Report(const std::string& line) {
	Log::WriteToConsoleAndLog(line + "\n");
}

PFN_vkGetInstanceProcAddr SystemInstanceProcAddr() {
	return reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
}

#if defined(KYTY_ANDROID)
// Driver package layout written by the launcher's importer: meta.json ("libraryName", "name",
// "driverVersion") next to the driver .so.
struct DriverPackage {
	std::string directory; // with trailing '/', as adrenotools concatenates directory + name
	std::string library_name;
	std::string display_name;
};

bool IsPlainSoName(const std::string& name) {
	return !name.empty() && name.find('/') == std::string::npos && name != "." && name != ".." &&
	       name.ends_with(".so");
}

std::optional<DriverPackage> ReadDriverPackage(const std::string& directory) {
	namespace fs = std::filesystem;
	DriverPackage package;
	package.directory = directory.ends_with('/') ? directory : directory + "/";

	std::ifstream meta_file(package.directory + "meta.json");
	if (!meta_file) {
		Report(fmt::format("GPU driver: {} has no meta.json", package.directory));
		return std::nullopt;
	}
	const auto meta = nlohmann::json::parse(meta_file, nullptr, /*allow_exceptions=*/false);
	if (!meta.is_object()) {
		Report(fmt::format("GPU driver: {}meta.json is not a JSON object", package.directory));
		return std::nullopt;
	}
	package.library_name = meta.value("libraryName", std::string());
	package.display_name = fmt::format("{} ({})", meta.value("name", std::string("unnamed")),
	                                   meta.value("driverVersion", std::string("unknown version")));
	if (!IsPlainSoName(package.library_name)) {
		Report(fmt::format("GPU driver: meta.json libraryName '{}' is not a .so file name",
		                   package.library_name));
		return std::nullopt;
	}
	std::error_code ec;
	if (!fs::is_regular_file(package.directory + package.library_name, ec)) {
		Report(fmt::format("GPU driver: {}{} is missing", package.directory, package.library_name));
		return std::nullopt;
	}
	return package;
}

// adrenotools loads its hook libraries from the APK's nativeLibraryDir, which is where libmain.so
// (this code) was extracted to.
std::optional<std::string> NativeLibraryDirectory() {
	Dl_info info {};
	if (dladdr(reinterpret_cast<void*>(&NativeLibraryDirectory), &info) == 0 ||
	    info.dli_fname == nullptr) {
		return std::nullopt;
	}
	return std::filesystem::path(info.dli_fname).parent_path().string() + "/";
}

PFN_vkGetInstanceProcAddr OpenCustomDriver(const std::string& directory) {
	const auto package = ReadDriverPackage(directory);
	if (!package) {
		return nullptr;
	}
	const auto hook_dir = NativeLibraryDirectory();
	if (!hook_dir) {
		Report("GPU driver: cannot locate the native library directory for the adrenotools hooks");
		return nullptr;
	}
	Report(fmt::format("GPU driver: loading {} from {}{}", package->display_name, package->directory,
	                   package->library_name));

	void* handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
	                                          nullptr, hook_dir->c_str(), package->directory.c_str(),
	                                          package->library_name.c_str(), nullptr, nullptr);
	if (handle == nullptr) {
		const char* error = dlerror();
		Report(fmt::format("GPU driver: adrenotools_open_libvulkan failed: {}",
		                   error != nullptr ? error : "no dlerror"));
		return nullptr;
	}
	auto* gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
	if (gipa == nullptr) {
		Report("GPU driver: the loaded driver exports no vkGetInstanceProcAddr");
		return nullptr;
	}
	return gipa;
}
#endif

} // namespace

void SetCustomDriverDirectory(std::string directory) {
	State().custom_directory = std::move(directory);
}

void SetPrintCaps(bool enabled) {
	State().print_caps = enabled;
}

bool PrintCapsEnabled() {
	return State().print_caps;
}

bool CustomDriverActive() {
	return State().custom_active;
}

PFN_vkGetInstanceProcAddr LoadInstanceProcAddr() {
	auto& state = State();
	if (!state.custom_directory.empty()) {
#if defined(KYTY_ANDROID)
		state.custom_gipa = OpenCustomDriver(state.custom_directory);
		if (state.custom_gipa != nullptr) {
			state.custom_active = true;
			return state.custom_gipa;
		}
		Report("GPU driver: custom driver unavailable; falling back to the system Vulkan driver");
#else
		Report("GPU driver: --gpu-driver is only supported on Android; using the system driver");
#endif
	}
	return SystemInstanceProcAddr();
}

PFN_vkGetInstanceProcAddr FallBackToSystemDriver() {
	auto& state = State();
	if (!state.custom_active) {
		return nullptr;
	}
	Report("GPU driver: the custom driver could not create a Vulkan instance; falling back to the "
	       "system Vulkan driver");
	state.custom_active = false;
	state.custom_gipa   = nullptr;
	return SystemInstanceProcAddr();
}

bool CreateSurface(SDL_Window* window, vk::Instance instance, vk::SurfaceKHR::CType* out_surface) {
	const auto native_instance = static_cast<vk::Instance::CType>(instance);
#if defined(KYTY_ANDROID)
	if (State().custom_active) {
		auto* native_window = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
		    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
		if (native_window == nullptr) {
			SDL_SetError("SDL window has no ANativeWindow");
			return false;
		}
		auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
		    State().custom_gipa(native_instance, "vkCreateAndroidSurfaceKHR"));
		if (create == nullptr) {
			SDL_SetError("custom driver has no vkCreateAndroidSurfaceKHR");
			return false;
		}
		VkAndroidSurfaceCreateInfoKHR create_info {};
		create_info.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
		create_info.window = native_window;
		const VkResult result = create(native_instance, &create_info, nullptr, out_surface);
		if (result != VK_SUCCESS) {
			SDL_SetError("vkCreateAndroidSurfaceKHR failed: %d", static_cast<int>(result));
			return false;
		}
		return true;
	}
#endif
	return SDL_Vulkan_CreateSurface(window, native_instance, nullptr, out_surface);
}

} // namespace Libs::Graphics::VulkanDriver
