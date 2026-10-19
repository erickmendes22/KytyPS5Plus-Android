#ifndef EMULATOR_SRC_KERNEL_THREADPLACEMENT_H_
#define EMULATOR_SRC_KERNEL_THREADPLACEMENT_H_

#include <cstdint>
#include <string_view>

// Host placement of the threads that gate frame submission (Android/Linux; inert elsewhere).
//
// KYTY_CPU_PLACE=<bits> (start value; the live knob cpu_place=<bits> overrides it while a game
// runs with KYTY_LIVE_KNOBS=1):
//   1  pin the critical threads to the cores with the highest current frequency limit
//      (scaling_max_freq, re-read every second; a hot Snapdragon 8 Elite caps its 4.32 GHz prime
//      cores below its 3.53 GHz cores)
//   2  an ADPF performance-hint session over the critical threads, told every frame that the
//      frame took longer than its target, so the scheduler raises their clocks
//   4  keep every other thread of the process off the fastest cores (with 1)
// Critical threads: the GPU command-processor thread, plus guest threads whose name contains one
// of the substrings in KYTY_CPU_CRITICAL (',' ';' or '|' separated).
namespace Kernel::ThreadPlacement {

enum class Role : uint8_t {
	GpuCommand, // GuestGpu::ThreadRun
	Guest,      // a named guest pthread (critical only if its name matches KYTY_CPU_CRITICAL)
};

// Records the calling thread and applies the current placement to it.
void RegisterCurrentThread(Role role, std::string_view name = {});

// Re-applies the placement when `mode` (the live value, -1 = keep the start value) changed.
// Called once per processed frame by the GPU thread; cheap when nothing changed.
void Poll(int64_t mode);

// ADPF: one frame's wall time, reported to the hint session when bit 2 is on.
void ReportFrame(int64_t frame_ns);

// Parses a KYTY_CPU_PLACE value (decimal bits 0..7); -1 for anything else. Exposed for tests.
[[nodiscard]] int ParseMode(std::string_view text);

} // namespace Kernel::ThreadPlacement

#endif // EMULATOR_SRC_KERNEL_THREADPLACEMENT_H_
