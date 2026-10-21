#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITSTATS_H_

#include <cstdint>
#include <string>

// Host submit accounting and tuning.
// KYTY_SUBMIT_STATS=1 (opt-in, with --perf-stats): "[perf-wait] submits:" - who ended each command
// buffer (CommandScheduler::Flush/Finish/FlushAndWait callers as libmain offsets, top 6) and the
// vkQueueSubmit time by the command buffer's work count (draws + dispatches), to tell a per-call
// cost from a per-work one.
// Live keys (KYTY_LIVE_KNOBS=1), defaults unchanged when absent:
//   idle_min=<n>      work items before an idle-GPU submit (kIdleSubmitMinWork, default 16)
//   coalesce_max=<n>  guest command buffers folded into one host submit (kMaxDeferredFlushes, 8)
//   irq_flush_ms=<n>  how long an end-of-pipe interrupt may wait for the next submit (8 ms)
namespace Libs::Graphics::SubmitStats {

[[nodiscard]] bool Enabled();
void NoteCaller(const void* return_address);
void NoteSubmit(uint32_t work, uint64_t submit_ns);
void Append(std::string& out, double per_frame);

[[nodiscard]] uint32_t IdleMinWork();
[[nodiscard]] uint32_t CoalesceMax();
[[nodiscard]] int64_t  InterruptFlushNs(int64_t fallback);

} // namespace Libs::Graphics::SubmitStats

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITSTATS_H_ */
