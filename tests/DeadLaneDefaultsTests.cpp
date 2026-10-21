#include "graphics/host_gpu/renderer/pipeline/deadLaneDefaults.h"

#include <cstdint>
#include <cstdio>

namespace {

using Libs::Graphics::DeadLaneFoldByDefault;
using Libs::Graphics::DeadLaneFoldDefaultShaders;

// Exact guest hashes: the measured RT lighting kernel only.
static_assert(DeadLaneFoldByDefault(0x6856320a9c6f89fdull));
static_assert(!DeadLaneFoldByDefault(0x6856320a9c6f89fcull));
static_assert(!DeadLaneFoldByDefault(0));
// Unmeasured tile-class siblings stay off until a measurement shows a gain.
static_assert(!DeadLaneFoldByDefault(0xd2c5a6652fd00b38ull));
static_assert(!DeadLaneFoldByDefault(0xf4387ca73f260f12ull));
static_assert(!DeadLaneFoldByDefault(0xa71743d8a83bbc5eull));

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DeadLaneDefaultsTests: failed: %s\n", message);
		g_failures++;
	}
}

} // namespace

int main() {
	Check(DeadLaneFoldDefaultShaders.size() == 1, "allow-list grew without a measurement");
	for (const auto hash: DeadLaneFoldDefaultShaders) {
		Check(DeadLaneFoldByDefault(hash), "listed hash not matched");
	}
	if (g_failures != 0) return 1;
	std::printf("DeadLaneDefaultsTests: ok\n");
	return 0;
}
