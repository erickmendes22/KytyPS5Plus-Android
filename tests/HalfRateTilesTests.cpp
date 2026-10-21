#include "graphics/shader/halfRateTiles.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

namespace H = Libs::Graphics::HalfRateTiles;

// 11 arrays of 64 words: valid, depth, 4 G-buffer, 4 color, luminance.
static_assert(H::kLdsDwords == 704);
// The kernel's 1920 LDS words plus the patch stay well under the 32 KiB compute LDS of Adreno.
static_assert((1920u + H::kLdsDwords) * 4u <= 32u * 1024u);
// Spare VGPRs are above the kernel's v0..v71 and inside the 256 the IR has.
static_assert(H::kRegOriginX > 71 && H::kRegExtentY < 256);

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HalfRateTilesTests: failed: %s\n", message);
		g_failures++;
	}
}

} // namespace

int main() {
	// Unset environment: the patch is off unless the live knob turns it on, and only for the RT
	// lighting kernel.
	unsetenv("KYTY_RT_HALF");
	Check(!H::Applies(H::kShaderHash, 0), "env unset must keep the patch off");
	Check(H::Applies(H::kShaderHash, 2), "live rt_half=1 must turn it on");
	Check(!H::Applies(H::kShaderHash, 1), "live rt_half=0 must keep it off");
	Check(!H::Applies(0xd2c5a6652fd00b38ull, 2), "sibling tile-class kernel must not match");
	Check(!H::Applies(H::kShaderHash ^ 1u, 2), "neighbouring hash must not match");
	if (g_failures != 0) return 1;
	std::printf("HalfRateTilesTests: ok\n");
	return 0;
}
