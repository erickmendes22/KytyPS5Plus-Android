#include "graphics/host_gpu/renderer/pipelineProbe.h"

#include <cstdio>

namespace {

using Libs::Graphics::PipelineProbe::Drain;
using Libs::Graphics::PipelineProbe::kWindow;
using Libs::Graphics::PipelineProbe::Model;

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", what);
		g_failures++;
	}
}

void WriteThenReadDrainsOnce() {
	Model m;
	m.BeginItem();
	m.OnGpuWrite(0x1000, 0x100);
	m.BeginItem();
	Check(m.OnSrtRead(0x1080, 4, false) == Drain::SrtWrite, "read of a queued item's write drains");
	Check(m.OnSrtRead(0x1080, 4, false) == Drain::Count, "after the drain the write is recorded");
	m.BeginItem();
	Check(m.OnSrtRead(0x1080, 4, false) == Drain::Count, "later items see it recorded too");
}

void OwnWritesDoNotDrain() {
	Model m;
	m.BeginItem();
	m.OnGpuWrite(0x2000, 0x40);
	Check(m.OnSrtRead(0x2000, 4, false) == Drain::Count,
	      "an item's walk runs before its own writes are recorded");
}

void DisjointReadsDoNotDrain() {
	Model m;
	m.BeginItem();
	m.OnGpuWrite(0x3000, 0x40);
	m.BeginItem();
	Check(m.OnSrtRead(0x3040, 4, false) == Drain::Count, "a read right after the range is clean");
	Check(m.OnSrtRead(0x2ffc, 4, false) == Drain::Count, "a read right before the range is clean");
	Check(m.OnSrtRead(0x2ffc, 8, false) == Drain::SrtWrite, "a read straddling the start drains");
}

void WindowAgesWritesOut() {
	Model m;
	m.BeginItem();
	m.OnGpuWrite(0x4000, 0x10);
	for (uint64_t i = 0; i < kWindow; i++) m.BeginItem();
	Check(m.OnSrtRead(0x4000, 4, false) == Drain::Count,
	      "writes older than the queue depth are recorded already");
}

void DmaWriterDrainsAnyRead() {
	Model m;
	m.BeginItem();
	m.MarkDmaWriter();
	m.BeginItem();
	Check(m.OnSrtRead(0x9000, 4, false) == Drain::SrtDma, "a queued BDA writer may write anything");
	Check(m.OnSrtRead(0x9000, 4, false) == Drain::Count, "once drained it is recorded");
	Model own;
	own.BeginItem();
	own.MarkDmaWriter();
	Check(own.OnSrtRead(0x9000, 4, false) == Drain::Count, "an item's own BDA use does not drain it");
}

void DirtyReadsDrain() {
	Model m;
	m.BeginItem();
	Check(m.OnSrtRead(0x5000, 4, true) == Drain::SrtDirty, "a GPU-dirty read needs a readback");
}

void ExplicitDrainClearsPending() {
	Model m;
	m.BeginItem();
	m.OnGpuWrite(0x6000, 0x10);
	m.MarkDmaWriter();
	m.BeginItem();
	m.OnDrain(); // e.g. a guest sync command
	Check(m.OnSrtRead(0x6000, 4, false) == Drain::Count, "a sync drain records everything before");
}

void RingKeepsNewestWrites() {
	Model m;
	m.BeginItem();
	for (uint64_t i = 0; i < 5000; i++) m.OnGpuWrite(0x100000 + i * 0x100, 0x10);
	m.BeginItem();
	Check(m.OnSrtRead(0x100000 + 4999 * 0x100, 4, false) == Drain::SrtWrite,
	      "the newest writes stay in the ring");
}

} // namespace

int main() {
	WriteThenReadDrainsOnce();
	OwnWritesDoNotDrain();
	DisjointReadsDoNotDrain();
	WindowAgesWritesOut();
	DmaWriterDrainsAnyRead();
	DirtyReadsDrain();
	ExplicitDrainClearsPending();
	RingKeepsNewestWrites();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("pipeline_probe_tests: ok\n");
	return 0;
}
