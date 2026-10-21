#include "graphics/guest_gpu/processWatchdog.h"

#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/renderer/gpuPipeline.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace Libs::Graphics::ProcessWatchdog {

namespace {

constexpr int64_t kStuckMs  = 2000;
constexpr int64_t kRepeatMs = 2000;

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

constexpr const char* kPhaseNames[] = {"idle", "process", "handler", "drain", "sync_cmd"};

void Report(const State& s, int64_t stuck_ms) {
	char head[512];
	const auto op    = s.last_opcode.load(std::memory_order_relaxed);
	const auto phase = std::min<uint32_t>(s.phase.load(std::memory_order_relaxed), 4u);
	std::snprintf(head, sizeof(head),
	              "[gpu-watchdog] process running %lld ms: submission=%llu phase=%s ib=0x%llx "
	              "offset=%u/%u dw depth=%u last_op=0x%02x r=0x%02x packets=%llu ib_switches=%llu "
	              "draws=%llu last_draw kind=%u count=%u instances=%u shader=0x%llx",
	              static_cast<long long>(stuck_ms),
	              static_cast<unsigned long long>(s.submission.load(std::memory_order_relaxed)),
	              kPhaseNames[phase],
	              static_cast<unsigned long long>(s.ib_address.load(std::memory_order_relaxed)),
	              s.ib_offset_dw.load(std::memory_order_relaxed), s.ib_size_dw.load(std::memory_order_relaxed),
	              s.ib_depth.load(std::memory_order_relaxed), op & 0xffu, (op >> 8u) & 0xffu,
	              static_cast<unsigned long long>(s.packets.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(s.ib_switches.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(s.draws.load(std::memory_order_relaxed)),
	              s.last_draw_kind.load(std::memory_order_relaxed), s.last_draw_count.load(std::memory_order_relaxed),
	              s.last_draw_instances.load(std::memory_order_relaxed),
	              static_cast<unsigned long long>(s.last_draw_shader.load(std::memory_order_relaxed)));
	std::string line = head;
	std::array<std::pair<uint32_t, uint32_t>, 256> ops {};
	for (uint32_t i = 0; i < 256; i++) ops[i] = {s.ops[i].load(std::memory_order_relaxed), i};
	std::partial_sort(ops.begin(), ops.begin() + 8, ops.end(), [](auto& a, auto& b) { return a.first > b.first; });
	line += " ops:";
	for (size_t i = 0; i < 8 && ops[i].first != 0; i++) {
		char part[32];
		std::snprintf(part, sizeof(part), " 0x%02x=%u", ops[i].second, ops[i].first);
		line += part;
	}
	char tail[160];
	const auto* back = GpuPipeline::BackIfCreated();
	auto&       pool = WalkFan::Pool();
	std::snprintf(tail, sizeof(tail), " record: queued=%zu busy=%d walk: queued=%zu running=%u workers=%u",
	              back != nullptr ? back->Queued() : size_t {0}, back != nullptr && back->Busy() ? 1 : 0,
	              pool.Queued(), pool.Running(), pool.Workers());
	line += tail;
	LOGF("%s\n", line.c_str());
	Common::PerfStats::EmitLine(line);
}

void Run() {
	auto&   s           = Get();
	int64_t last_report = 0;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		const auto start = s.start_ns.load(std::memory_order_acquire);
		if (start == 0) continue;
		const auto now      = NowNs();
		const auto stuck_ms = (now - start) / 1'000'000;
		if (stuck_ms < kStuckMs) continue;
		if (last_report != 0 && (now - last_report) / 1'000'000 < kRepeatMs) continue;
		last_report = now;
		Report(s, stuck_ms);
	}
}

} // namespace

State& Get() {
	static State state;
	return state;
}

void BeginProcess() {
	auto& s = Get();
	s.packets.store(0, std::memory_order_relaxed);
	s.ib_switches.store(0, std::memory_order_relaxed);
	s.draws.store(0, std::memory_order_relaxed);
	for (auto& op: s.ops) op.store(0, std::memory_order_relaxed);
	s.submission.fetch_add(1, std::memory_order_relaxed);
	s.phase.store(static_cast<uint32_t>(Phase::Process), std::memory_order_relaxed);
	s.start_ns.store(NowNs(), std::memory_order_release);
}

void EndProcess() {
	auto& s = Get();
	s.start_ns.store(0, std::memory_order_release);
	s.phase.store(static_cast<uint32_t>(Phase::Idle), std::memory_order_relaxed);
}

void Start() {
	static const bool started = [] {
		std::thread(Run).detach();
		return true;
	}();
	(void)started;
}

} // namespace Libs::Graphics::ProcessWatchdog
