// Driver-dependent shader knob defaults (graphics/host_gpu/renderer/pipeline/knobDefaults.h):
// KYTY_SWIZZLE_RELATIVE and KYTY_FOLD_USER_DEPTH are on by default with Mesa Turnip only.
#include "graphics/host_gpu/renderer/pipeline/knobDefaults.h"

#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;

static void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "KnobDefaultsTests: %s\n", text);
		std::abort();
	}
}

int main() {
	// KYTY_SWIZZLE_RELATIVE (default = turnip).
	Check(KnobDefaults::Toggle(nullptr, true), "unset: on with Turnip");
	Check(!KnobDefaults::Toggle(nullptr, false), "unset: off elsewhere");
	Check(KnobDefaults::Toggle("", true) && !KnobDefaults::Toggle("", false), "empty: the default");
	Check(!KnobDefaults::Toggle("0", true), "=0 opts out with Turnip");
	Check(KnobDefaults::Toggle("1", false), "=1 opts in elsewhere");
	Check(KnobDefaults::Toggle("00", false), "only the exact 0 opts out");

	// KYTY_FOLD_USER_DEPTH.
	Check(KnobDefaults::FoldUserDepth(nullptr, true) == KnobDefaults::kFoldUserDepthTurnip, "unset: 16 with Turnip");
	Check(KnobDefaults::kFoldUserDepthTurnip == 16u, "the Turnip default is 16");
	Check(KnobDefaults::FoldUserDepth(nullptr, false) == 0u, "unset: the whole cone elsewhere");
	Check(KnobDefaults::FoldUserDepth("", true) == 16u, "empty: the default");
	Check(KnobDefaults::FoldUserDepth("0", true) == 0u, "=0 opts out with Turnip");
	Check(KnobDefaults::FoldUserDepth("32", true) == 32u, "an explicit depth wins");
	Check(KnobDefaults::FoldUserDepth("8", false) == 8u, "an explicit depth applies elsewhere");
	std::puts("KnobDefaultsTests: ok");
	return 0;
}
