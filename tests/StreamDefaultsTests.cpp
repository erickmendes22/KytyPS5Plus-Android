#include "graphics/host_gpu/renderer/cache/streamDefaults.h"

#include <cstdio>

namespace {

namespace SD = Libs::Graphics::StreamDefaults;
using SD::MemoMode;

// Unset: on with the measured values on Android, off elsewhere.
static_assert(SD::PromoteThreshold(nullptr, true) == 2);
static_assert(SD::PromoteThreshold(nullptr, false) == 0);
static_assert(SD::CachedStream(nullptr, true));
static_assert(!SD::CachedStream(nullptr, false));
static_assert(SD::SrtMemo(nullptr, true) == MemoMode::Auto);
static_assert(SD::SrtMemo(nullptr, false) == MemoMode::Off);

// An explicit value wins on every platform; =0 opts out.
static_assert(SD::PromoteThreshold("0", true) == 0);
static_assert(SD::PromoteThreshold("4", true) == 4);
static_assert(SD::PromoteThreshold("16", false) == 16);
static_assert(SD::PromoteThreshold("-3", true) == 0);
static_assert(SD::PromoteThreshold("abc", true) == 0);
static_assert(SD::PromoteThreshold("", true) == 0);
static_assert(!SD::CachedStream("0", true));
static_assert(SD::CachedStream("1", false));
static_assert(!SD::CachedStream("yes", true));
static_assert(SD::SrtMemo("0", true) == MemoMode::Off);
static_assert(SD::SrtMemo("1", true) == MemoMode::On);
static_assert(SD::SrtMemo("1", false) == MemoMode::On);
static_assert(SD::SrtMemo("auto", false) == MemoMode::Auto);
static_assert(SD::SrtMemo("", true) == MemoMode::Off);

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "StreamDefaultsTests: failed: %s\n", message);
		g_failures++;
	}
}

} // namespace

int main() {
	// The same cases at run time (env strings are not literals there).
	char promote[] = "2";
	Check(SD::PromoteThreshold(promote, false) == 2, "runtime promote value");
	char memo[] = "auto";
	Check(SD::SrtMemo(memo, false) == MemoMode::Auto, "runtime memo auto");
	Check(SD::kAndroid == (SD::PromoteThreshold(nullptr, SD::kAndroid) > 0), "platform default for promote");
	if (g_failures != 0) return 1;
	std::printf("StreamDefaultsTests: ok\n");
	return 0;
}
