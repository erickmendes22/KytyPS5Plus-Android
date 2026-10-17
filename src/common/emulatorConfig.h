#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Config {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Config";
	static constexpr auto        initialize = Config::Initialize;
	static constexpr auto        shutdown   = Config::Shutdown;
};

enum class ShaderOptimizationType { None, Size, Performance };

enum class LogDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

using Keymap = std::vector<std::string>;
using ControllerColor = std::array<uint8_t, 3>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id != USER_ID_EVERYONE && user_id != USER_ID_SYSTEM;
}

constexpr uint32_t RENDER_PASS_MODE_AUTO = UINT32_MAX;
// --render-scale bounds: below 1/4 the scaled targets lose too much; above 2x gains nothing.
constexpr float MIN_RENDER_SCALE = 0.25f;
constexpr float MAX_RENDER_SCALE = 2.0f;
// --render-scale-flags bits (diagnostics; each disables one part of the scaling).
constexpr uint32_t RENDER_SCALE_FLAG_KEEP_FRAGCOORD = 1u << 0u; // no gl_FragCoord unscale
constexpr uint32_t RENDER_SCALE_FLAG_KEEP_TEXELS    = 1u << 1u; // no texel coordinate scaling
constexpr uint32_t RENDER_SCALE_FLAG_NO_DEPTH       = 1u << 2u; // depth images stay 1:1
constexpr uint32_t RENDER_SCALE_FLAG_LOG            = 1u << 3u; // log every backing resize

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = DEFAULT_USER_ID;
	std::string            audio_input_device;
	std::optional<ControllerColor> controller_color;
	uint32_t               controller_speaker_volume      = 50;
	uint32_t               controller_vibration_intensity = 100;
	PresentMode            present_mode                = PresentMode::Mailbox;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	bool                   hide_cursor_enabled         = false;
	bool                   splash_enabled              = true;
	bool                   vr_enabled                  = false;
	bool                   amd_cpu_enabled             = false;
	bool                   network_connected           = false;
	uint32_t               vblank_frequency            = 60;
	// AvPlayer keeps this many superseded video frames unrecycled (UINT32_MAX = built-in default).
	uint32_t               video_retire_depth          = UINT32_MAX;
	// FFmpeg video decoder thread count (0 = automatic).
	uint32_t               video_decoder_threads       = 0;
	// 0: record render passes immediately (one pass per emulated mesh draw); 1: defer passes to
	// secondary command buffers; 2: 1 + run mesh-emulation dispatches ahead of the open pass.
	// RENDER_PASS_MODE_AUTO: currently 0 (deferred passes measured slower on Turnip).
	uint32_t               render_pass_mode            = RENDER_PASS_MODE_AUTO;
	// Image layout policy. 0: legacy (depth attachment layout follows the depth-write state,
	// every image barrier ends the open render pass); 1: a depth attachment the draw does not
	// sample stays in a writable layout, and a depth attachment the draw samples read-only is
	// acquired with the sampled access (no per-draw barrier); 2: 1 + with deferred render
	// passes, barriers for images the open pass does not use are recorded ahead of it.
	// Default 0: on Turnip v32 the merged passes ran slower (Astro Bot intro 5.9 -> 5.5 fps,
	// the hot volumetric draw 3.3 -> 6.5 ms each) although passes fell from 133 to 96 per frame.
	uint32_t               layout_mode                 = 0;
	// Internal render scale: frame-sized render targets and depth buffers are allocated at this
	// fraction of their guest size (viewport, scissor, gl_FragCoord and texel fetches follow);
	// 1.0 renders at the guest's native size. See renderer/image/renderScale.h.
	float                  render_scale                = 1.0f;
	// --render-scale-flags: diagnostics for the render scale (bits of RenderScaleFlag*).
	uint32_t               render_scale_flags          = 0;
	// Translate guest shaders and build host pipelines on worker threads; a graphics draw whose
	// pipeline is not ready yet is skipped for that frame (compute dispatches always wait).
	// Default on for Android, where a translation stalls the GPU thread for 0.3-4.6 s.
#if defined(__ANDROID__)
	bool                   async_shaders               = true;
#else
	bool                   async_shaders               = false;
#endif
	// Shader worker threads (0 = automatic: cores / 2 - 1, at most 3).
	uint32_t               shader_threads              = 0;
	// --submit-thread: vkQueueSubmit runs on its own thread so the GPU (command processor)
	// thread keeps translating PM4 while the driver queues the previous command buffer. On
	// Turnip/KGSL a submit costs 2-16 ms of the GPU thread (28-34 per frame in Astro Bot).
#if defined(__ANDROID__)
	bool                   submit_thread               = true;
#else
	bool                   submit_thread               = false;
#endif
	// --timeline-tracker: one thread waits for each timeline tick in turn and publishes the
	// GPU's progress, replacing vkGetSemaphoreCounterValue polls on the working threads (40-55
	// per frame, about 1 ms each on Turnip/KGSL, which walks its emulated timeline).
#if defined(__ANDROID__)
	bool                   timeline_tracker            = true;
#else
	bool                   timeline_tracker            = false;
#endif
	// --gpu-frames-in-flight: how many AgcSuspendPoint frames the guest may run ahead of the
	// host GPU's completion; 1 waits at each suspend point for the previous frame's GPU work,
	// 0 never waits there (the guest's own flip and label waits pace it).
	uint32_t               gpu_frames_in_flight        = 1;
	// --submit-coalesce: a guest graphics submission does not end a host command buffer while
	// more graphics submissions are queued (flips, suspend points and downloads still do), so
	// several guest submissions share one vkQueueSubmit. EOP interrupts wait up to 8 ms instead
	// of 1 ms for a batch; nothing waits past the moment the GPU thread runs out of work.
#if defined(__ANDROID__)
	bool                   submit_coalesce             = true;
#else
	bool                   submit_coalesce             = false;
#endif
	// --targeted-readback: a synchronous readback of GPU-written buffer memory waits only for the
	// command buffer that last wrote those bytes (not at all once it completed) and, where the
	// buffer's memory is host-visible (unified-memory GPUs), reads it in place instead of
	// recording a copy into the open command buffer and waiting for the whole submit backlog.
#if defined(__ANDROID__)
	bool                   targeted_readback           = true;
#else
	bool                   targeted_readback           = false;
#endif
	// --idle-submit: between PM4 packets, submit the open command buffer as soon as the GPU has
	// run out of submitted work and it holds a batch of draws/dispatches, instead of letting the
	// GPU idle until the guest submission (or a coalesced batch of them) ends. After a readback
	// has drained the GPU, the rest of the frame then overlaps with its own recording.
#if defined(__ANDROID__)
	bool                   idle_submit                 = true;
#else
	bool                   idle_submit                 = false;
#endif
	// --gpu-indirect: DRAW_INDIRECT / DRAW_INDEX_INDIRECT(_MULTI) execute as Vulkan indirect draws
	// that read the guest arguments on the GPU (a compute pass clamps them first) instead of a
	// synchronous CPU readback of arguments that a shader wrote earlier in the same frame. The
	// vertex shader reads the base vertex/instance user SGPRs from gl_BaseVertex/gl_BaseInstance.
	// Draws it cannot express (tessellation, fast-launch or multi-record emulated mesh draws, 8-bit
	// or scanned-restart indices, rect/quad lists, metadata operations) keep the CPU path.
	// 2: also keep the CPU path for records whose last draw was empty (GPU feedback): an empty
	// GPU draw still costs its compute and render pass, which is the slower choice while the GPU
	// is the bottleneck (Astro Bot's logo scene), the faster one otherwise.
#if defined(__ANDROID__)
	uint32_t               gpu_indirect                = 1;
#else
	uint32_t               gpu_indirect                = 0;
#endif
	// --gpu-memory-budget: device-memory budget (MiB) the texture and buffer cache collectors keep
	// to; -1 = auto (MemAvailable at start minus a CPU-side reserve, at most 4 GiB, lowered at run
	// time when VK_EXT_memory_budget reports little headroom), 0 = legacy thresholds from the
	// driver-reported heap size.
#if defined(__ANDROID__)
	int32_t                gpu_memory_budget_mb        = -1;
#else
	int32_t                gpu_memory_budget_mb        = 0;
#endif
	// --gpu-evict-written: with a budget, the collector may also download GPU-written images
	// (render targets of an earlier scene, tiled ones included) to guest memory and evict them
	// when long unused under pressure. Off: only images guest memory already holds are evicted
	// (allocation failures still download and evict).
	bool                   gpu_evict_written           = false;
	// --vma-block-size: VMA's preferred block size (MiB) for large heaps; 0 = VMA default (256).
	// Smaller blocks empty out (and return to the system) sooner after eviction.
#if defined(__ANDROID__)
	uint32_t               vma_block_size_mb           = 64;
#else
	uint32_t               vma_block_size_mb           = 0;
#endif
	// --gpu-fatal-dialog: on VK_ERROR_DEVICE_LOST / out of device memory show a message box with
	// the reason (for up to 30 s) before exiting.
#if defined(__ANDROID__)
	bool                   gpu_fatal_dialog            = true;
#else
	bool                   gpu_fatal_dialog            = false;
#endif
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	// Non-empty: write every host SPIR-V module and pipeline description here (shader_checker).
	std::filesystem::path  shader_dump_folder;
	LogDirection           printf_direction            = LogDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	bool                   profiler_enabled            = false;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   trophy_enabled              = true;
	uint32_t               trophy_duration_seconds     = 6;
	bool                   playgo_hack_enabled         = false;
	bool                   skip_notice_screen          = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
	// Ordinary boots need a non-null argv[0]; --guest-exec supplies the caller's arguments.
	std::vector<std::string> guest_arguments {""};
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
const std::string& GetAudioInputDevice();
const std::optional<ControllerColor>& GetControllerColor();
uint32_t GetControllerSpeakerVolume();
uint32_t GetControllerVibrationIntensity();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     HdrEnabled();
bool     HideCursorEnabled();
bool     VrEnabled();
bool     AmdCpuEnabled();
bool     IsNetworkConnected();
uint32_t GetVblankFrequency();
uint32_t GetVideoRetireDepth();
uint32_t GetVideoDecoderThreads();
uint32_t GetRenderPassMode();
uint32_t GetLayoutMode();
float    GetRenderScale();
uint32_t GetRenderScaleFlags();
bool     AsyncShadersEnabled();
uint32_t GetShaderThreads();
bool     SubmitThreadEnabled();
bool     TimelineTrackerEnabled();
uint32_t GetGpuFramesInFlight();
bool     SubmitCoalesceEnabled();
bool     TargetedReadbackEnabled();
bool     IdleSubmitEnabled();
bool     GpuIndirectEnabled();
bool     GpuIndirectPredictsEmpty();
int32_t  GetGpuMemoryBudgetMb();
bool     GpuEvictWrittenEnabled();
uint32_t GetVmaBlockSizeMb();
bool     GpuFatalDialogEnabled();
constexpr uint32_t MAX_GPU_FRAMES_IN_FLIGHT = 4;
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
LogDirection           GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();

// Empty when --dump-shaders is not given.
const std::filesystem::path& GetShaderDumpFolder();

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool SyncRawImageBuffersEnabled();
bool TessellationEnabled();
bool TrophyEnabled();
uint32_t GetTrophyDurationSeconds();
bool PlayGoHackEnabled();
bool SkipNoticeScreen();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();
const std::vector<std::string>& GetGuestArguments();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
