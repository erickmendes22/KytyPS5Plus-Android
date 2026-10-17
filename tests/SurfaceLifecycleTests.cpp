#include "graphics/presentation/window/surfaceLifecycle.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::SurfaceLifecycle;
using Action = SurfaceLifecycle::Action;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "SurfaceLifecycleTests: failed: %s\n", text);
		std::abort();
	}
}

void TestStartsPresenting() {
	SurfaceLifecycle s;
	Check(s.HasSurface(), "startup surface exists");
	Check(s.Next(0) == Action::Present, "foreground with a surface presents");
	Check(s.Next(1000) == Action::Present, "keeps presenting");
}

void TestBackgroundTearsDownThenDrops() {
	SurfaceLifecycle s;
	s.EnterBackground();
	Check(s.Next(10) == Action::Teardown, "background tears the surface down");
	s.TornDown();
	Check(!s.HasSurface(), "no surface after teardown");
	for (uint64_t t = 10; t < 5000; t += 16) {
		Check(s.Next(t) == Action::Drop, "background without a surface drops frames");
	}
}

void TestForegroundRebuildsOnFirstPresent() {
	SurfaceLifecycle s;
	s.EnterBackground();
	Check(s.Next(0) == Action::Teardown, "teardown");
	s.TornDown();
	s.EnterForeground();
	Check(s.Next(100) == Action::Rebuild, "first present after foreground rebuilds");
	s.Rebuilt(true, 100);
	Check(s.Next(116) == Action::Present, "presents after the rebuild");
}

void TestFailedRebuildRetriesLater() {
	SurfaceLifecycle s;
	s.EnterBackground();
	(void)s.Next(0);
	s.TornDown();
	s.EnterForeground();
	Check(s.Next(100) == Action::Rebuild, "rebuild");
	s.Rebuilt(false, 100); // the new ANativeWindow is not there yet
	Check(s.FailedRebuilds() == 1, "one failed rebuild counted");
	Check(s.Next(101) == Action::Drop, "drops right after a failed rebuild");
	Check(s.Next(100 + SurfaceLifecycle::RetryDelayMs - 1) == Action::Drop, "waits the delay");
	Check(s.Next(100 + SurfaceLifecycle::RetryDelayMs) == Action::Rebuild, "retries after delay");
	s.Rebuilt(true, 400);
	Check(s.FailedRebuilds() == 0, "success resets the failure count");
	Check(s.Next(401) == Action::Present, "presents");
}

void TestQuickBackgroundForegroundStillRebuilds() {
	// Both events land between two presents: the old surface points at the released window.
	SurfaceLifecycle s;
	s.EnterBackground();
	s.EnterForeground();
	Check(s.Next(0) == Action::Teardown, "stale surface is torn down");
	s.TornDown();
	Check(s.Next(0) == Action::Rebuild, "and rebuilt at once");
	s.Rebuilt(true, 0);
	Check(s.Next(1) == Action::Present, "presents");
}

void TestBackgroundDuringRebuild() {
	SurfaceLifecycle s;
	s.EnterBackground();
	(void)s.Next(0);
	s.TornDown();
	s.EnterForeground();
	Check(s.Next(10) == Action::Rebuild, "rebuild");
	s.EnterBackground(); // races the rebuild
	s.Rebuilt(true, 10);
	Check(s.Next(11) == Action::Teardown, "surface built for an older generation is torn down");
}

void TestLostWhilePresentingWithoutLifecycleEvent() {
	// Surface lost before the lifecycle watch ran, and the recreation failed.
	SurfaceLifecycle s;
	s.Lost(50);
	Check(s.Next(51) == Action::Drop, "drops after the loss");
	s.EnterBackground();
	Check(s.Next(50 + SurfaceLifecycle::RetryDelayMs) == Action::Drop, "background keeps dropping");
	s.EnterForeground();
	Check(s.Next(50 + SurfaceLifecycle::RetryDelayMs) == Action::Rebuild, "foreground rebuilds");
}

void TestLostWithoutBackgroundRetries() {
	SurfaceLifecycle s;
	s.Lost(0);
	Check(s.Next(SurfaceLifecycle::RetryDelayMs) == Action::Rebuild,
	      "a loss without lifecycle events is retried after the delay");
}

} // namespace

int main() {
	TestStartsPresenting();
	TestBackgroundTearsDownThenDrops();
	TestForegroundRebuildsOnFirstPresent();
	TestFailedRebuildRetriesLater();
	TestQuickBackgroundForegroundStillRebuilds();
	TestBackgroundDuringRebuild();
	TestLostWhilePresentingWithoutLifecycleEvent();
	TestLostWithoutBackgroundRetries();
	std::puts("SurfaceLifecycleTests: all passed");
	return 0;
}
