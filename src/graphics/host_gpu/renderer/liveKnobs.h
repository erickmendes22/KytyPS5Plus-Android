#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LIVEKNOBS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LIVEKNOBS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Diagnostics knobs that can change while a game runs (KYTY_LIVE_KNOBS=1 only; inert otherwise).
// Once a second the renderer re-reads "_live.txt" in the working directory (the app's files
// directory on Android), so a long run can bisect a scene without restarting:
//
//   skip=<hex prefixes>   drop draws whose vertex or pixel shader, and dispatches whose compute
//                         shader, has a matching guest hash (KYTY_PIPELINE_STATS syntax)
//   marks=1               --perf-stats: one GPU timestamp per draw, labelled by its path
//                         (draw / draw-gs / draw-rect / draw-mesh / draw-tess) and pixel shader,
//                         with the vertex shader appended
//   shake=<seconds>       shake the pad (synthetic accelerometer + gyroscope) for 1-60 s, starting
//                         when the file changes; for motion-only prompts (Astro Bot's intro "shake
//                         to fix Astro") on a tablet with on-screen controls
//
// A missing file means no skips and no draw marks.
namespace Libs::Graphics::LiveKnobs {

struct Snapshot {
	std::vector<std::string> skip;
	bool                     draw_marks = false;
	int                      shake_seconds = 0;
};

// Parses the file contents. Unknown keys and bad hash prefixes are ignored. Exposed for tests.
[[nodiscard]] Snapshot Parse(std::string_view text);

// The current knobs (re-read at most once a second). Never null.
[[nodiscard]] const Snapshot& Current();

[[nodiscard]] bool SkipShader(uint64_t guest_hash);
[[nodiscard]] bool DrawMarks();

} // namespace Libs::Graphics::LiveKnobs

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LIVEKNOBS_H_
