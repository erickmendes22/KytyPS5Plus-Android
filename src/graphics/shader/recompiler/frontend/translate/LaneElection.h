#pragma once

#include "graphics/shader/recompiler/ir/IREmitter.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// The lane that performs a wave-wide memory update on behalf of the wave (DS_ORDERED_COUNT).
struct LaneElection {
	IR::U1  active;   // some lane of `exec` is active
	IR::U32 selected; // the elected lane (0 when no lane is active)
};

// The lowest lane of `exec`. With `exclude_helpers` (KYTY_PS_LIVE_ELECT, pixel stage) the lowest
// non-helper lane of `exec` instead: a helper invocation's atomics have no effect, and guest EXEC
// never holds helper lanes outside WQM. Falls back to the lowest `exec` lane when every active
// lane is a helper.
[[nodiscard]] LaneElection ElectFirstLane(IR::IREmitter& ir, IR::U1 exec, bool exclude_helpers);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
