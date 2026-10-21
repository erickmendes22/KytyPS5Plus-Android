#include "graphics/host_gpu/renderer/registerOverride.h"

#include <cstdio>
#include <thread>

namespace {

using Libs::Graphics::BasicOverrideScope;
using Libs::Graphics::BasicRegisterOverride;
using Libs::Graphics::PickRegisters;

struct Ctx {
	int value = 0;
};
struct Ucfg {
	int value = 0;
};
struct Sh {
	int value = 0;
};
using Override = BasicRegisterOverride<Ctx, Ucfg, Sh>;
thread_local Override t_override {};

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", what);
		g_failures++;
	}
}

// What a record job does: copy the live blocks when the item is queued, read them later.
struct Snapshot {
	Ctx  ctx;
	Ucfg ucfg;
	Sh   sh;
};

void SnapshotSurvivesLiveWrites() {
	Ctx      live {1};
	Snapshot item {live, {}, {}};
	live.value = 2; // the front keeps parsing
	const BasicOverrideScope scope(t_override, Override {&item.ctx, &item.ucfg, &item.sh});
	Check(PickRegisters(t_override.context, live).value == 1, "the job reads its snapshot");
}

void NoOverrideReadsLive() {
	Ctx live {5};
	Check(PickRegisters(t_override.context, live).value == 5, "without a job the live block is read");
}

void ScopesNestAndRestore() {
	Ctx      live {0};
	Snapshot a {{1}, {}, {}};
	Snapshot b {{2}, {}, {}};
	{
		const BasicOverrideScope outer(t_override, Override {&a.ctx, &a.ucfg, &a.sh});
		{
			const BasicOverrideScope inner(t_override, Override {&b.ctx, &b.ucfg, &b.sh});
			Check(PickRegisters(t_override.context, live).value == 2, "inner scope wins");
		}
		Check(PickRegisters(t_override.context, live).value == 1, "outer scope restored");
	}
	Check(t_override.context == nullptr, "no override after both scopes");
}

void OverrideIsPerThread() {
	Ctx      live {7};
	Snapshot item {{3}, {}, {}};
	const BasicOverrideScope scope(t_override, Override {&item.ctx, &item.ucfg, &item.sh});
	int other = -1;
	std::thread front([&] { other = PickRegisters(t_override.context, live).value; });
	front.join();
	Check(other == 7, "another thread (the front) still reads the live block");
	Check(PickRegisters(t_override.context, live).value == 3, "this thread reads the snapshot");
}

} // namespace

int main() {
	SnapshotSurvivesLiveWrites();
	NoOverrideReadsLive();
	ScopesNestAndRestore();
	OverrideIsPerThread();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("register_override_tests: ok\n");
	return 0;
}
