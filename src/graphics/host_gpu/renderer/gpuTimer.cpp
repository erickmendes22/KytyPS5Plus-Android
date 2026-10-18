#include "graphics/host_gpu/renderer/gpuTimer.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_clock_probe_spv.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/gpuFatal.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

namespace {

constexpr size_t kLogTop  = 6;
constexpr size_t kFileTop = 60;

// Clock probe defaults: ~0.7 ms of FMA work on an Adreno 830 at its top clock.
constexpr long     kProbeDefaultMs         = 2000;
constexpr uint32_t kProbeDefaultIterations = 4096;
constexpr uint32_t kProbeDefaultGroups     = 1024;

std::atomic<uint32_t> g_instances {0};

// Live timers, for the device-loss dump (GpuFatal runs on whichever thread saw the loss).
std::mutex              g_timers_mutex;
std::vector<GpuTimer*>  g_timers;

void DumpAllInFlight(vk::Result result) {
	const std::lock_guard lock(g_timers_mutex);
	for (auto* timer: g_timers) {
		timer->DumpInFlight(result);
	}
}

// Regions listed per in-flight command buffer: this many before the first unfinished one and
// after it (all of them when the driver returns no availability).
constexpr uint32_t kHangContextBefore = 6;
constexpr uint32_t kHangContextAfter  = 24;
constexpr uint32_t kHangMaxBuffers    = 6;

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

namespace {

constexpr std::array<const char*, 5> kCrumbKindNames {"begin", "pass", "cs", "mesh-cs",
                                                      "indirect-fixup"};

// Index into kCrumbKindNames ("cs" matches every compute kind, e.g. "cs-indirect").
int CrumbKindIndex(const char* kind) {
	if (kind == nullptr) {
		return -1;
	}
	for (size_t i = 0; i < kCrumbKindNames.size(); i++) {
		if (std::strcmp(kind, kCrumbKindNames[i]) == 0) {
			return static_cast<int>(i);
		}
	}
	return std::strncmp(kind, "cs", 2) == 0 ? 2 : -1;
}

uint32_t ParseCrumbKinds(const char* spec) {
	if (spec == nullptr || *spec == '\0') {
		return ~0u;
	}
	uint32_t    kinds = 0;
	std::string list  = spec;
	size_t      start = 0;
	while (start <= list.size()) {
		// ',' ':' or '+' (EXTRA_ARGS splits on commas).
		const auto end   = std::min(list.find_first_of(",:+", start), list.size());
		const auto token = list.substr(start, end - start);
		const auto index = CrumbKindIndex(token.c_str());
		if (index >= 0) {
			kinds |= 1u << static_cast<uint32_t>(index);
		}
		start = end + 1;
	}
	return kinds;
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
	if (EnvLong("KYTY_GPU_BREADCRUMBS", 0) != 0) {
		CreateBreadcrumbs();
	}
	{
		const std::lock_guard lock(g_timers_mutex);
		g_timers.push_back(this);
	}
	AddGpuFatalHook(DumpAllInFlight);
}

GpuTimer::~GpuTimer() {
	{
		const std::lock_guard lock(g_timers_mutex);
		std::erase(g_timers, this);
	}
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
	if (m_crumb_buffer != nullptr) {
		m_graphics.device.destroyBuffer(m_crumb_buffer, nullptr);
	}
	if (m_crumb_memory != nullptr) {
		m_graphics.device.unmapMemory(m_crumb_memory);
		m_graphics.device.freeMemory(m_crumb_memory, nullptr);
	}
}

void GpuTimer::CreateBreadcrumbs() {
	const auto device = m_graphics.device;
	vk::BufferCreateInfo info {};
	info.size  = kCrumbSlots * sizeof(uint32_t);
	info.usage = vk::BufferUsageFlagBits::eTransferDst;
	if (device.createBuffer(&info, nullptr, &m_crumb_buffer) != vk::Result::eSuccess) {
		m_crumb_buffer = nullptr;
		return;
	}
	vk::MemoryRequirements requirements {};
	device.getBufferMemoryRequirements(m_crumb_buffer, &requirements);
	const auto properties = m_graphics.physical_device.getMemoryProperties();
	const auto wanted     = vk::MemoryPropertyFlagBits::eHostVisible |
	                    vk::MemoryPropertyFlagBits::eHostCoherent;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
		    (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			type = i;
			break;
		}
	}
	vk::MemoryAllocateInfo allocate {};
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	void* map = nullptr;
	if (type == UINT32_MAX ||
	    device.allocateMemory(&allocate, nullptr, &m_crumb_memory) != vk::Result::eSuccess ||
	    device.bindBufferMemory(m_crumb_buffer, m_crumb_memory, 0) != vk::Result::eSuccess ||
	    device.mapMemory(m_crumb_memory, 0, VK_WHOLE_SIZE, {}, &map) != vk::Result::eSuccess) {
		LOGF("[gpu-hang] breadcrumbs unavailable\n");
		device.destroyBuffer(m_crumb_buffer, nullptr);
		m_crumb_buffer = nullptr;
		return;
	}
	m_crumbs      = static_cast<volatile uint32_t*>(map);
	m_crumb_kinds = ParseCrumbKinds(std::getenv("KYTY_CRUMB_KINDS"));
	LOGF("[gpu-hang] timer%u breadcrumbs on (%u slots, kinds 0x%x)\n", m_instance, kCrumbSlots,
	     m_crumb_kinds);
}

bool GpuTimer::CrumbKind(const char* kind) const {
	const auto index = CrumbKindIndex(kind);
	return index >= 0 && (m_crumb_kinds & (1u << static_cast<uint32_t>(index))) != 0;
}

void GpuTimer::WriteBreadcrumb(vk::CommandBuffer buffer, uint32_t value, bool fenced) {
	if (m_crumbs == nullptr) {
		return;
	}
	if (!fenced) {
		// Resets the command buffer's slot only; the next fenced write orders after it.
		buffer.fillBuffer(m_crumb_buffer, m_current.crumb_slot * sizeof(uint32_t),
		                  sizeof(uint32_t), value);
		return;
	}
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0,
	                       nullptr);
	buffer.fillBuffer(m_crumb_buffer, m_current.crumb_slot * sizeof(uint32_t), sizeof(uint32_t),
	                  value);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	                       {}, 1, &after, 0, nullptr, 0, nullptr);
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
	m_current.crumb_slot = m_crumb_serial++ % kCrumbSlots;
	WriteBreadcrumb(buffer, 0, CrumbKind("begin"));
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
	// Only regions closed outside a render pass (a pass end, a dispatch).
	if (m_crumbs != nullptr && CrumbKind(kind)) {
		WriteBreadcrumb(buffer, m_current.count - 1u);
	}
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
	{
		const std::lock_guard lock(m_pending_mutex);
		m_pending.push_back(std::move(m_current));
	}
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
		const std::lock_guard lock(m_pending_mutex);
		m_pending.pop_front();
	}
	CollectProbes();
	MaybeDump();
}

void GpuTimer::DumpInFlight(vk::Result result) {
	const std::lock_guard lock(m_pending_mutex);
	const auto now  = Common::PerfStats::Detail::MonotonicNs();
	const auto last = m_master.LastProgressNs();
	LOGF("[gpu-hang] timer%u %s: known_gpu_tick=%" PRIu64 " submitted_tick=%" PRIu64
	     " last GPU progress %.0f ms ago, %zu timed command buffers in flight\n",
	     m_instance, vk::to_string(result).c_str(), m_master.KnownGpuTick(),
	     m_master.SubmittedTick(), last == 0 ? -1.0 : static_cast<double>(now - last) / 1e6,
	     m_pending.size());
	uint32_t shown = 0;
	for (const auto& recording: m_pending) {
		if (recording.tick <= m_master.KnownGpuTick()) {
			continue;
		}
		if (shown++ == kHangMaxBuffers) {
			break;
		}
		// Per query: the timestamp and its availability word.
		std::vector<uint64_t> words(size_t {recording.count} * 2u, 0);
		const auto query = m_graphics.device.getQueryPoolResults(
		    recording.pool, 0, recording.count, words.size() * sizeof(uint64_t), words.data(),
		    2 * sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		const bool known = query == vk::Result::eSuccess || query == vk::Result::eNotReady;
		uint32_t   first = recording.count; // first region whose end stamp is unavailable
		if (known) {
			for (uint32_t i = 0; i < recording.count; i++) {
				if (words[i * 2u + 1u] == 0) {
					first = i;
					break;
				}
			}
		}
		if (!known && m_crumbs != nullptr) {
			// The last breadcrumb the GPU wrote: every region up to it finished.
			const uint32_t crumb = m_crumbs[recording.crumb_slot];
			if (crumb < recording.count) {
				first = crumb + 1u;
			}
			LOGF("[gpu-hang]  tick=%" PRIu64 " breadcrumb=%u\n", recording.tick, crumb);
		}
		const bool located = known || m_crumbs != nullptr;
		LOGF("[gpu-hang]  tick=%" PRIu64 " regions=%u queries=%s first_unfinished=%u\n",
		     recording.tick, recording.count - 1u, vk::to_string(query).c_str(), first);
		const uint32_t begin = located && first > kHangContextBefore ? first - kHangContextBefore : 1u;
		const uint32_t end   = located ? std::min(recording.count, first + kHangContextAfter)
		                               : recording.count;
		for (uint32_t i = std::max(begin, 1u); i < end; i++) {
			const auto& label = recording.labels[i];
			const bool  done  = known && i < first;
			const auto  us    = done ? static_cast<double>((words[i * 2u] - words[(i - 1u) * 2u]) &
			                                               m_valid_mask) * m_period_ns / 1e3
			                         : -1.0;
			LOGF("[gpu-hang]   %s#%u %s %.0fus\n", i == first ? ">>" : "  ", i,
			     LabelName(label.kind != nullptr ? label.kind : "start", label.hash, label.pass).c_str(),
			     us);
		}
	}
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
