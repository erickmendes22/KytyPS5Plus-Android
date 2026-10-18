#include "graphics/host_gpu/renderer/gpuTimer.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_clock_probe_spv.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics {

namespace {

constexpr size_t kLogTop  = 6;
constexpr size_t kFileTop = 60;

// Clock probe defaults: ~0.7 ms of FMA work on an Adreno 830 at its top clock.
constexpr long     kProbeDefaultMs         = 2000;
constexpr uint32_t kProbeDefaultIterations = 4096;
constexpr uint32_t kProbeDefaultGroups     = 1024;

std::atomic<uint32_t> g_instances {0};

long EnvLong(const char* name, long fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return fallback;
	}
	char*      end    = nullptr;
	const long parsed = std::strtol(value, &end, 10);
	return end != nullptr && *end == '\0' && parsed >= 0 ? parsed : fallback;
}

std::string LabelName(const char* kind, uint64_t hash, const GpuPassInfo& pass) {
	char buf[128];
	if (pass.width != 0) {
		std::snprintf(buf, sizeof(buf), "%s %ux%u c%u%s 0x%016" PRIx64, kind, pass.width,
		              pass.height, pass.colors, pass.depth ? "d" : "", hash);
	} else {
		std::snprintf(buf, sizeof(buf), "%s 0x%016" PRIx64, kind, hash);
	}
	std::string name = buf;
	if (pass.aux != 0) {
		std::snprintf(buf, sizeof(buf), " vs 0x%016" PRIx64, pass.aux);
		name += buf;
	}
	return name;
}

// KYTY_PERF_GPU_TOP=<n>: regions per [perf-gpu] log line (default kLogTop).
size_t LogTop() {
	static const size_t top = [] {
		const char* value = std::getenv("KYTY_PERF_GPU_TOP");
		const long  n     = value != nullptr ? std::strtol(value, nullptr, 10) : 0;
		return n > 0 ? std::min<size_t>(static_cast<size_t>(n), kFileTop) : kLogTop;
	}();
	return top;
}

} // namespace

GpuTimer::GpuTimer(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	if (!Common::PerfStats::Enabled() || graphics.queue_family == static_cast<uint32_t>(-1)) {
		return;
	}
	const auto families = graphics.physical_device.getQueueFamilyProperties();
	if (graphics.queue_family >= families.size()) {
		return;
	}
	const auto valid_bits = families[graphics.queue_family].timestampValidBits;
	const auto period     = graphics.physical_device_properties.limits.timestampPeriod;
	if (valid_bits == 0 || period <= 0.0f) {
		Common::PerfStats::EmitLine("[perf-gpu] the queue has no timestamps; GPU busy time off");
		return;
	}
	m_valid_mask   = valid_bits >= 64 ? ~uint64_t {0} : (uint64_t {1} << valid_bits) - 1;
	m_period_ns    = static_cast<double>(period);
	m_enabled      = true;
	m_last_dump_ns = Common::PerfStats::Detail::MonotonicNs();
	m_start_ns     = m_last_dump_ns;
	m_instance     = g_instances.fetch_add(1, std::memory_order_relaxed);
	if (m_instance == 0) {
		CreateProbe();
	}
}

GpuTimer::~GpuTimer() {
	for (auto pool: m_all_pools) {
		m_graphics.device.destroyQueryPool(pool, nullptr);
	}
	if (m_probe_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_probe_pool, nullptr);
	}
	if (m_probe_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_probe_pipeline, nullptr);
	}
	if (m_probe_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_probe_layout, nullptr);
	}
}

void GpuTimer::CreateProbe() {
	const long interval_ms = EnvLong("KYTY_GPU_PROBE_MS", kProbeDefaultMs);
	if (interval_ms == 0) {
		return;
	}
	m_probe_iterations = static_cast<uint32_t>(
	    std::clamp(EnvLong("KYTY_GPU_PROBE_ITERATIONS", kProbeDefaultIterations), 1L, 1L << 20));
	m_probe_groups = static_cast<uint32_t>(
	    std::clamp(EnvLong("KYTY_GPU_PROBE_GROUPS", kProbeDefaultGroups), 1L, 65535L));
	const auto device = m_graphics.device;
	const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(ProbeParams)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	RequireVulkanSuccess(device.createPipelineLayout(&layout_info, nullptr, &m_probe_layout),
	                     "create GPU clock probe pipeline layout");
	const auto module = CompileSPV(GPU_CLOCK_PROBE_SPV, device);
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = m_probe_layout;
	const auto result = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr,
	                                                  &m_probe_pipeline);
	device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create GPU clock probe pipeline");
	SetVulkanObjectNameF(device, m_probe_pipeline, "GPU Clock Probe");

	vk::QueryPoolCreateInfo pool_info {};
	pool_info.queryType  = vk::QueryType::eTimestamp;
	pool_info.queryCount = kProbeSlots * 2;
	RequireVulkanSuccess(device.createQueryPool(&pool_info, nullptr, &m_probe_pool),
	                     "create GPU clock probe query pool");
	m_probe_interval_ns = int64_t {interval_ms} * 1'000'000;
	char line[160];
	std::snprintf(line, sizeof(line),
	              "[perf-probe] GPU clock probe every %ld ms: %u groups x 64 x %u iterations",
	              interval_ms, m_probe_groups, m_probe_iterations);
	Common::PerfStats::EmitLine(line);
}

// At the start of a command buffer: wait for everything submitted before, time the probe alone,
// and keep the work after it from overlapping it.
void GpuTimer::RecordProbe(vk::CommandBuffer buffer) {
	if (m_probe_pipeline == nullptr || m_probe_recorded) {
		return;
	}
	const auto now = Common::PerfStats::Detail::MonotonicNs();
	if (now - m_last_probe_ns < m_probe_interval_ns) {
		return;
	}
	const uint32_t slot = m_probe_next_slot;
	for (const auto& pending: m_probe_pending) {
		if (pending.slot == slot) {
			return; // all slots in flight; try again at the next command buffer
		}
	}
	m_last_probe_ns   = now;
	m_probe_next_slot = (slot + 1) % kProbeSlots;

	vk::MemoryBarrier2 fence {};
	fence.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	fence.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &fence;

	buffer.resetQueryPool(m_probe_pool, slot * 2, 2);
	buffer.pipelineBarrier2(dependency);
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_probe_pool, slot * 2);
	buffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_probe_pipeline);
	const ProbeParams params {.sink = 0, .iterations = m_probe_iterations, .never = -1.0f};
	buffer.pushConstants(m_probe_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
	                     &params);
	buffer.dispatch(m_probe_groups, 1, 1);
	buffer.pipelineBarrier2(dependency);
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_probe_pool, slot * 2 + 1);
	m_probe_recorded      = true;
	m_probe_recorded_slot = slot;
}

void GpuTimer::CollectProbes() {
	while (!m_probe_pending.empty() && m_master.IsFree(m_probe_pending.front().tick)) {
		const auto slot = m_probe_pending.front().slot;
		m_probe_pending.pop_front();
		std::array<uint64_t, 2> stamps {};
		const auto result = m_graphics.device.getQueryPoolResults(
		    m_probe_pool, slot * 2, 2, sizeof(stamps), stamps.data(), sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64);
		if (result != vk::Result::eSuccess) {
			LOGF("[perf-probe] reading probe slot %u failed: %s\n", slot,
			     vk::to_string(result).c_str());
			continue;
		}
		const uint64_t ticks = (stamps[1] - stamps[0]) & m_valid_mask;
		const double   ms    = static_cast<double>(ticks) * m_period_ns / 1e6;
		const double   t     = static_cast<double>(Common::PerfStats::Detail::MonotonicNs() -
		                                           m_start_ns) / 1e9;
		char line[96];
		std::snprintf(line, sizeof(line), "[perf-probe] t=%.1f gpu_ms=%.3f", t, ms);
		Common::PerfStats::EmitLine(line);
	}
}

std::pair<vk::QueryPool, uint32_t> GpuTimer::AcquirePool() {
	if (!m_free_pools.empty()) {
		const auto pool = m_free_pools.back();
		m_free_pools.pop_back();
		return pool;
	}
	vk::QueryPoolCreateInfo create {};
	create.queryType  = vk::QueryType::eTimestamp;
	create.queryCount = kQueriesPerBuffer;
	vk::QueryPool pool = nullptr;
	if (m_graphics.device.createQueryPool(&create, nullptr, &pool) != vk::Result::eSuccess) {
		return {nullptr, 0};
	}
	m_all_pools.push_back(pool);
	return {pool, kQueriesPerBuffer};
}

void GpuTimer::Begin(vk::CommandBuffer buffer) {
	if (!m_enabled) {
		return;
	}
	EXIT_IF(m_recording);
	Collect();
	RecordProbe(buffer);
	m_current                = {};
	const auto [pool, dirty] = AcquirePool();
	if (pool == nullptr) {
		return;
	}
	m_current.pool = pool;
	buffer.resetQueryPool(pool, 0, dirty);
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_current.pool, 0);
	m_current.count = 1;
	m_current.labels.push_back({});
	m_recording = true;
}

void GpuTimer::Mark(vk::CommandBuffer buffer, const char* kind, uint64_t hash,
                    const GpuPassInfo& pass) {
	// Keep the last query for End().
	if (!m_recording || m_current.count + 1 >= kQueriesPerBuffer) {
		return;
	}
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_current.pool,
	                      m_current.count++);
	m_current.labels.push_back({kind, hash, pass});
}

void GpuTimer::End(vk::CommandBuffer buffer) {
	if (!m_recording) {
		return;
	}
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_current.pool,
	                      m_current.count++);
	m_current.labels.push_back({"other", 0});
}

void GpuTimer::Submitted(uint64_t tick) {
	if (m_probe_recorded) {
		m_probe_recorded = false;
		m_probe_pending.push_back({m_probe_recorded_slot, tick});
	}
	if (!m_recording) {
		return;
	}
	m_recording    = false;
	m_current.tick = tick;
	m_pending.push_back(std::move(m_current));
	m_current = {};
}

void GpuTimer::Collect() {
	if (!m_enabled) {
		return;
	}
	// Only the GPU progress the renderer already observed: querying the timeline here would
	// perturb what is being measured (it is a kernel call on some drivers).
	std::vector<uint64_t> stamps;
	while (!m_pending.empty() && m_master.IsFree(m_pending.front().tick)) {
		auto& recording = m_pending.front();
		stamps.resize(recording.count);
		const auto result = m_graphics.device.getQueryPoolResults(
		    recording.pool, 0, recording.count, stamps.size() * sizeof(uint64_t), stamps.data(),
		    sizeof(uint64_t), vk::QueryResultFlagBits::e64);
		if (result == vk::Result::eSuccess) {
			Account(recording, stamps);
		}
		m_free_pools.emplace_back(recording.pool, recording.count);
		m_pending.pop_front();
	}
	CollectProbes();
	MaybeDump();
}

void GpuTimer::Account(const Recording& recording, const std::vector<uint64_t>& stamps) {
	const auto to_ns = [this](uint64_t later, uint64_t earlier) {
		const uint64_t ticks = (later - earlier) & m_valid_mask;
		return static_cast<uint64_t>(static_cast<double>(ticks) * m_period_ns);
	};
	const auto busy = to_ns(stamps[recording.count - 1], stamps[0]);
	Common::PerfStats::Count(Common::PerfStats::Counter::GpuBusyNs, busy);
	Common::PerfStats::Count(Common::PerfStats::Counter::GpuSubmitsTimed);
	m_window_busy_ns += busy;
	for (uint32_t i = 1; i < recording.count; i++) {
		const auto& label = recording.labels[i];
		auto& cost = m_costs[LabelName(label.kind, label.hash, label.pass)];
		cost.ns += to_ns(stamps[i], stamps[i - 1]);
		cost.count++;
		cost.draws += label.pass.draws;
		cost.indices += label.pass.indices;
	}
}

void GpuTimer::MaybeDump() {
	const auto now = Common::PerfStats::Detail::MonotonicNs();
	if (now - m_last_dump_ns < int64_t {kDumpSeconds} * 1'000'000'000) {
		return;
	}
	const double seconds = static_cast<double>(now - m_last_dump_ns) / 1e9;
	m_last_dump_ns       = now;
	std::vector<std::pair<std::string, Cost>> sorted(m_costs.begin(), m_costs.end());
	std::sort(sorted.begin(), sorted.end(),
	          [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
	if (sorted.empty()) {
		return;
	}
	char head[96];
	std::snprintf(head, sizeof(head), "[perf-gpu%u] busy=%.1fms/s top regions (ms/s, count/s):",
	              m_instance, static_cast<double>(m_window_busy_ns) / 1e6 / seconds);
	std::string line = head;
	char        file_name[32];
	std::snprintf(file_name, sizeof(file_name), "_perf_gpu%u.txt", m_instance);
	if (FILE* out = std::fopen(file_name, "w"); out != nullptr) {
		std::fprintf(out, "# last %.1f s, GPU busy %.1f ms/s; region ms/s, count/s, us/each, "
		                  "draws/each, Kindices/each\n",
		             seconds, static_cast<double>(m_window_busy_ns) / 1e6 / seconds);
		for (size_t i = 0; i < std::min(sorted.size(), kFileTop); i++) {
			const auto& [name, cost] = sorted[i];
			const auto each = static_cast<double>(cost.count);
			std::fprintf(out, "%8.2f %7.1f %8.1f %6.1f %8.1f  %s\n",
			             static_cast<double>(cost.ns) / 1e6 / seconds,
			             static_cast<double>(cost.count) / seconds,
			             static_cast<double>(cost.ns) / 1e3 / each,
			             static_cast<double>(cost.draws) / each,
			             static_cast<double>(cost.indices) / 1e3 / each, name.c_str());
		}
		std::fclose(out);
	}
	for (size_t i = 0; i < std::min(sorted.size(), LogTop()); i++) {
		const auto& [name, cost] = sorted[i];
		char part[192];
		std::snprintf(part, sizeof(part), " [%s]=%.1f(%.0f)", name.c_str(),
		              static_cast<double>(cost.ns) / 1e6 / seconds,
		              static_cast<double>(cost.count) / seconds);
		line += part;
	}
	Common::PerfStats::EmitLine(line);
	m_costs.clear();
	m_window_busy_ns = 0;
}

} // namespace Libs::Graphics
