// GpuPipeline::RecordThread handoff tests. Run once with KYTY_PIPE_BATCH=0 and once with =1: jobs
// run once each, in enqueue order; Drain() returns only after every queued job finished; a full
// queue blocks the front without losing a wakeup; slow and fast jobs mixed never deadlock.

#include "graphics/host_gpu/renderer/gpuPipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

using Libs::Graphics::GpuPipeline::RecordThread;

void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "GpuPipelineBatchTests: failed: %s\n", message);
		std::abort();
	}
}

// Aborts the process if the tests hang (a lost wakeup).
void StartHangGuard(std::atomic<bool>& done) {
	std::thread([&done] {
		for (int i = 0; i < 1200 && !done.load(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!done.load()) {
			std::fprintf(stderr, "GpuPipelineBatchTests: failed: hang (lost wakeup)\n");
			std::abort();
		}
	}).detach();
}

void TestOrderAndDrain(RecordThread& back) {
	constexpr uint64_t kJobs = 200000;
	uint64_t           next  = 0; // touched by the record thread only, read after Drain
	std::atomic<bool>  in_order {true};
	for (uint64_t i = 0; i < kJobs; i++) {
		back.Enqueue([&next, &in_order, i] {
			if (next != i) {
				in_order.store(false);
			}
			next = i + 1;
		});
		if (i % 997 == 0) {
			back.Drain();
			Check(next == i + 1, "T1 Drain returns after every queued job ran");
			Check(back.Idle(), "T1 idle after Drain");
		}
	}
	back.Drain();
	Check(next == kJobs, "T1 every job ran once");
	Check(in_order.load(), "T1 jobs run in enqueue order");
	std::puts("PASS T1 order and drain");
}

void TestFullQueue(RecordThread& back) {
	// Slow jobs fill the queue so Enqueue blocks; a Drain right after must still see all of them.
	std::atomic<uint32_t> ran {0};
	for (int round = 0; round < 20; round++) {
		for (uint32_t i = 0; i < RecordThread::kMaxQueued * 3; i++) {
			const bool slow = i % 16 == 0;
			back.Enqueue([&ran, slow] {
				if (slow) {
					std::this_thread::sleep_for(std::chrono::microseconds(200));
				}
				ran.fetch_add(1, std::memory_order_relaxed);
			});
		}
		back.Drain();
		Check(ran.load() == (round + 1) * RecordThread::kMaxQueued * 3, "T2 drain after a full queue");
	}
	std::puts("PASS T2 full queue");
}

void TestBurstsWithIdleGaps(RecordThread& back) {
	// The record thread falls asleep between bursts; the next Enqueue must wake it.
	std::atomic<uint32_t> ran {0};
	uint32_t              queued = 0;
	for (int burst = 0; burst < 300; burst++) {
		for (int i = 0; i < 1 + burst % 7; i++) {
			back.Enqueue([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
			queued++;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(burst % 3 == 0 ? 300 : 0));
		if (burst % 5 == 0) {
			back.Drain();
			Check(ran.load() == queued, "T3 drain after an idle gap");
		}
	}
	back.Drain();
	Check(ran.load() == queued, "T3 every burst ran");
	std::puts("PASS T3 bursts with idle gaps");
}

} // namespace

int main() {
	const char* mode = std::getenv("KYTY_PIPE_BATCH");
	std::printf("GpuPipelineBatchTests: KYTY_PIPE_BATCH=%s\n", mode != nullptr ? mode : "(unset)");
	std::atomic<bool> done {false};
	StartHangGuard(done);
	{
		RecordThread back;
		TestOrderAndDrain(back);
		TestFullQueue(back);
		TestBurstsWithIdleGaps(back);
	}
	done.store(true);
	std::puts("GpuPipelineBatchTests: all cases passed");
	return 0;
}
