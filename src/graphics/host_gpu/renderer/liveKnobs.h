#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LIVEKNOBS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LIVEKNOBS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Diagnostics knobs that can change while a game runs (KYTY_LIVE_KNOBS=1 only; inert otherwise).
// Once a second the renderer re-reads "_live.txt" in the working directory (the app's files
// directory on Android), so a long run can bisect a scene without restarting:
//
//   skip=<hex prefixes>   drop draws whose vertex or pixel shader, and dispatches whose compute
//                         shader, has a matching guest hash (KYTY_PIPELINE_STATS syntax)
//   mesh-skip-draw=<hex prefixes>
//                         emulated mesh draws of matching mesh shaders still run their compute
//                         stage (the ring is written) but drop the vertex-pull draw
//   mesh-skip-cs=<hex prefixes>
//                         emulated mesh draws of matching mesh shaders skip the compute dispatch;
//                         the draw reads whatever the ring range holds (stale, never out of the
//                         reservation: the vertex pull clamps indices)
//   marks=1               --perf-stats: one GPU timestamp per draw, labelled by its path
//                         (draw / draw-gs / draw-rect / draw-mesh / draw-tess) and pixel shader,
//                         with the vertex shader appended
//   shake=<seconds>       shake the pad (synthetic accelerometer + gyroscope) for 1-60 s, starting
//                         when the file changes; for motion-only prompts (Astro Bot's intro "shake
//                         to fix Astro") on a tablet with on-screen controls
//   pad=<script>          timed left stick + buttons (libs/padScript.h), starting when the file
//                         changes, e.g. pad=3000:128,0;400:128,0,cross;2000:128,0
//   <name>=<integer>      any other key with a decimal integer value, read with Int() (A/B
//                         phases of CPU-side options, e.g. cpu_place=1)
//
// A missing file means no skips and no draw marks.
namespace Libs::Graphics::LiveKnobs {

struct Snapshot {
	std::vector<std::string> skip;
	std::vector<std::string> mesh_skip_draw;
	std::vector<std::string> mesh_skip_cs;
	bool                     draw_marks = false;
	int                      shake_seconds = 0;
	std::string              pad_script;
	// Other keys with integer values, in file order.
	std::vector<std::pair<std::string, int64_t>> ints;
};

// Parses the file contents. Unknown keys and bad hash prefixes are ignored. Exposed for tests.
[[nodiscard]] Snapshot Parse(std::string_view text);

// The current knobs (re-read at most once a second). Never null.
[[nodiscard]] const Snapshot& Current();

[[nodiscard]] bool SkipShader(uint64_t guest_hash);
// mesh-skip-draw= / mesh-skip-cs= for an emulated mesh stage's guest hash.
[[nodiscard]] bool SkipMeshDraw(uint64_t mesh_hash);
[[nodiscard]] bool SkipMeshDispatch(uint64_t mesh_hash);
[[nodiscard]] bool DrawMarks();

// The integer value of <name> in the current file, or `fallback` when it is absent (or live knobs
// are off).
[[nodiscard]] int64_t Int(std::string_view name, int64_t fallback);

} // namespace Libs::Graphics::LiveKnobs

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LIVEKNOBS_H_
