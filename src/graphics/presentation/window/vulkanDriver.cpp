#include "graphics/presentation/window/vulkanDriver.h"

#include "common/logging/log.h"
#include "common/processExitHooks.h"
#include "graphics/presentation/window/vulkanDriverLoader.h"

#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <cstdlib>
#include <fmt/format.h>
#include <string>
#include <utility>

#if defined(KYTY_ANDROID)
#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
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

// Lock-free so the release can run from exit hooks and signal handlers.
std::atomic<bool> g_turbo_requested {false};
std::atomic<bool> g_turbo_active {false};

void Report(const std::string& line);

#if defined(KYTY_ANDROID)
// adrenotools_set_turbo without its error swallowing: KGSL_PROP_PWRCTRL 0 = keep the GPU at its
// maximum clock with DCVS off, 1 = normal power control. Returns 0 or errno. Async-signal-safe.
int SetKgslTurbo(bool turbo) noexcept {
	constexpr unsigned int KgslPropPwrctrl = 0xE;
	struct KgslProperty {
		unsigned int type;
		void*        value;
		size_t       sizebytes;
	};
	constexpr unsigned long KgslSetProperty = _IOW(0x09, 0x32, KgslProperty);
	unsigned int enable = turbo ? 0u : 1u;
	KgslProperty property {KgslPropPwrctrl, &enable, sizeof(enable)};
	const int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return errno;
	}
	const int result = ioctl(fd, KgslSetProperty, &property) == 0 ? 0 : errno;
	close(fd);
	return result;
}

// The policy runs before the emulator log exists, so it also goes to logcat (tag KytyGpu).
void ReportClock(const char* what, int error) {
	const auto line = error == 0 ? fmt::format("GPU clock: {}", what)
	                             : fmt::format("GPU clock: {} FAILED: {}", what, std::strerror(error));
	__android_log_write(error == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "KytyGpu", line.c_str());
	Report(line);
}

// KGSL_PROP_PWRCTRL goes through kgsl_pwrscale_disable/enable, which end in
// devfreq_suspend_device/devfreq_resume_device. devfreq counts those calls: a resume restarts the
// GPU governor only once suspend_count drops below 1. Every turbo request a run never released
// (force-stop, low-memory killer) leaves +1 behind, which one release cannot undo. A release with
// nothing to undo only resets the governor to its initial frequency, so the default-clock policy
// sends enough of them to drain any realistic leak. kgsl queues each request as a work item and
// merges back-to-back ones, hence the spacing.
constexpr int  KgslReleaseDrainCount = 64;
constexpr auto KgslRequestSpacing    = std::chrono::milliseconds(5);

void DrainLeakedGpuTurbo() {
	std::thread([] {
		int failures   = 0;
		int last_error = 0;
		for (int i = 0; i < KgslReleaseDrainCount; i++) {
			if (const int error = SetKgslTurbo(false); error != 0) {
				failures++;
				last_error = error;
			}
			std::this_thread::sleep_for(KgslRequestSpacing);
		}
		const auto what = fmt::format("default clocks: sent {} releases for leftover --gpu-turbo "
		                              "requests ({} failed)",
		                              KgslReleaseDrainCount, failures);
		ReportClock(what.c_str(), failures == KgslReleaseDrainCount ? last_error : 0);
	}).detach();
}

// Takes the clock with a release first, so a request left by a killed run (or a foreground return
// whose background release was missed) stays balanced: off/on leaves the count where it was.
int AcquireGpuTurboBalanced() noexcept {
	(void)SetKgslTurbo(false);
	std::this_thread::sleep_for(KgslRequestSpacing);
	return SetKgslTurbo(true);
}

// Serializes the turbo request between the lifecycle watch and the keeper thread (not the exit
// release, which may run in a signal handler).
std::mutex& TurboMutex() {
	static std::mutex mutex;
	return mutex;
}

// KGSL_PROP_PWRCTRL "on" is kgsl_pwrscale_disable(turbo): the devfreq governor is suspended and
// the power level set to 0, clamped by the thermal and PM QoS limits of the moment
// (_adjust_pwrlevel). A cap that arrives later (kgsl_cooling_set_cur_state,
// pmqos_max_notifier_call) lowers the active level, and when it is lifted the level is only
// re-clamped, not raised: with the governor suspended the GPU keeps the capped clock for good
// (Astro Bot: ~3x slower GPU work after one Thermal Status 1 episode). Re-taking the request
// lets the level climb back to the highest one the current limits allow; while a cap holds it
// changes nothing.
constexpr long kTurboReassertDefaultMs = 2000;

long TurboReassertMs() {
	const char* value = std::getenv("KYTY_GPU_TURBO_REASSERT_MS");
	if (value == nullptr || *value == '\0') {
		return kTurboReassertDefaultMs;
	}
	char*      end    = nullptr;
	const long parsed = std::strtol(value, &end, 10);
	return end != nullptr && *end == '\0' && parsed >= 0 ? parsed : kTurboReassertDefaultMs;
}

void StartGpuTurboKeeper() {
	const long interval_ms = TurboReassertMs();
	if (interval_ms == 0) {
		ReportClock("--gpu-turbo: re-assert off (KYTY_GPU_TURBO_REASSERT_MS=0)", 0);
		return;
	}
	ReportClock(fmt::format("--gpu-turbo: re-asserting every {} ms in the foreground", interval_ms)
	                .c_str(),
	            0);
	std::thread([interval_ms] {
		uint64_t reasserts = 0;
		int      reported  = 0;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
			const std::lock_guard lock(TurboMutex());
			if (!g_turbo_active.load(std::memory_order_acquire)) {
				continue; // background or exiting
			}
			const int error = AcquireGpuTurboBalanced();
			if (!g_turbo_active.load(std::memory_order_acquire)) {
				// The exit release ran in between: undo the request taken above (an extra
				// release is harmless, a leaked request is not).
				(void)SetKgslTurbo(false);
				continue;
			}
			reasserts++;
			if (error != 0 && reported++ < 4) {
				ReportClock(fmt::format("--gpu-turbo: re-assert #{}", reasserts).c_str(), error);
			}
		}
	}).detach();
}
#endif

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

void RequestGpuTurbo() {
	g_turbo_requested.store(true, std::memory_order_release);
}

bool GpuTurboRequested() {
	return g_turbo_requested.load(std::memory_order_acquire);
}

void ApplyGpuClockPolicy() {
#if defined(KYTY_ANDROID)
	if (!GpuTurboRequested()) {
		if (std::getenv("KYTY_GPU_CLOCK_KEEP") != nullptr) {
			// Test hook: shows whether a previous run's exit path released the clock.
			ReportClock("default clocks: leftover request left as is (KYTY_GPU_CLOCK_KEEP)", 0);
			return;
		}
		// The KGSL request is device-global and outlives the process that made it; runs that were
		// killed (force-stop, low-memory killer) could not undo theirs, so every run without
		// --gpu-turbo starts by draining them, off the startup path.
		DrainLeakedGpuTurbo();
		return;
	}
	(void)Common::AddProcessExitHook(ReleaseGpuTurbo);
	{
		const std::lock_guard lock(TurboMutex());
		g_turbo_active.store(true, std::memory_order_release);
		ReportClock("--gpu-turbo: maximum clocks (reset on exit and in the background)",
		            AcquireGpuTurboBalanced());
	}
	StartGpuTurboKeeper();
#else
	if (GpuTurboRequested()) {
		Report("GPU driver: --gpu-turbo is only supported on Android");
	}
#endif
}

void SuspendGpuTurbo() noexcept {
#if defined(KYTY_ANDROID)
	const std::lock_guard lock(TurboMutex());
	if (g_turbo_active.exchange(false, std::memory_order_acq_rel)) {
		ReportClock("--gpu-turbo: default clocks while in the background", SetKgslTurbo(false));
	}
#endif
}

void ResumeGpuTurbo() noexcept {
#if defined(KYTY_ANDROID)
	const std::lock_guard lock(TurboMutex());
	if (GpuTurboRequested() && !g_turbo_active.exchange(true, std::memory_order_acq_rel)) {
		ReportClock("--gpu-turbo: maximum clocks again in the foreground",
		            AcquireGpuTurboBalanced());
	}
#endif
}

void ReleaseGpuTurbo() noexcept {
#if defined(KYTY_ANDROID)
	// Exit hooks may run in a signal handler: no logging here.
	if (g_turbo_active.exchange(false, std::memory_order_acq_rel)) {
		(void)SetKgslTurbo(false);
	}
#endif
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
