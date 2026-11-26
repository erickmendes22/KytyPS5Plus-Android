#include "graphics/host_gpu/renderer/demonsDominantGeometry.h"
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include "graphics/shader/demonsPcDppPolicy.h"
#include "graphics/host_gpu/renderer/demonsQueueProbe.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <string_view>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>

namespace Libs::Graphics::ShaderDump {

namespace {

struct DumpedModule {
	std::string file;
	uint64_t    guest_hash = 0;
};

struct State {
	std::mutex                                     mutex;
	std::unordered_map<VkShaderModule, DumpedModule> modules;
	std::unordered_set<std::string>                ds_probe_guest_code;
	std::unordered_set<std::string>                ds_guest_attempted;
	uint64_t                                       extra_cs_bytes = 0;
	uint64_t                                       decision_graphics_bytes = 0;
	uint64_t                                       decision_guest_bytes = 0;
	uint64_t                                       decision_pipeline_bytes = 0;
	uint64_t                                       capture_failures = 0;
	nlohmann::json                                 failure_records = nlohmann::json::array();
	bool                                           device_written = false;
	bool                                           failed         = false;
};

State& GetState() {
	static State state;
	return state;
}

const std::filesystem::path& Root() {
	return Config::GetShaderDumpFolder();
}

// With Android's automatic Demon trace, save the affected compute pipelines
// and observed G-buffer fragment shaders. The diagnostic ZIP exports these exact
// SPIR-V binaries and Vulkan pipeline JSON without needing RenderDoc.
bool DsLightingShader(uint64_t hash) {
	static constexpr std::array<uint64_t, 14> watched {{
	    DemonsPcDppPolicy::RayProducer,
	    0xbcbcc6ba1e68c974ull, 0xacce347eef557bd2ull,
	    0x2f493aadc660cf03ull, 0x567d21ad45386c8dull,
	    0xc03afccb96a13c5dull, 0x3dfe402653925fbfull, 0x6cbfe18ea2005263ull,
	    0x1ce1a858f62b5a11ull, 0xc2e01fbe98f5ebabull,
	    0x23056085c1120461ull, 0xfabe0bcb275a3005ull,
	    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
	}};
	return hash == 0xcb3b9c1ea64fe876ull || hash == 0xc387ee3c610ed66bull ||
	       hash == 0xb89a7139ba15ee52ull || hash == 0xf0c140ea8df8f532ull || hash == 0x3b7e54e11ee65120ull ||
	       hash == 0xfb18d110fab8234eull || hash == 0x2454ae3e3a6cbdabull ||
	       hash == 0xb16650e4c9acfdddull || hash == 0xd45e1b87d6741f63ull ||
	       hash == DemonsQueueProbe::TileProducer ||
	       std::ranges::find(DemonsQueueProbe::Shaders, hash) != DemonsQueueProbe::Shaders.end() ||
	       std::find(watched.begin(), watched.end(), hash) != watched.end();
}

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return false;
	}
	file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	file.close();
	return static_cast<bool>(file);
}

bool ExistingFileMatches(const std::filesystem::path& path, const void* data, size_t size) {
	std::error_code ec;
	if (std::filesystem::file_size(path, ec) != size || ec) return false;
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	std::array<char, 64 * 1024> buffer {};
	const auto* expected = static_cast<const char*>(data);
	for (size_t offset = 0; offset < size;) {
		const auto count = std::min(buffer.size(), size - offset);
		file.read(buffer.data(), static_cast<std::streamsize>(count));
		if (file.gcount() != static_cast<std::streamsize>(count) ||
		    std::memcmp(buffer.data(), expected + offset, count) != 0) return false;
		offset += count;
	}
	return true;
}

void WriteCaptureStatus(State& state) {
	if (!DsDecisionCapture()) return;
	const auto text = nlohmann::json {
	    {"schema", 1}, {"guest_files_saved", state.ds_probe_guest_code.size()},
	    {"guest_bytes_written", state.decision_guest_bytes},
	    {"graphics_spv_bytes_written", state.decision_graphics_bytes},
	    {"pipeline_json_bytes_written", state.decision_pipeline_bytes},
	    {"extra_compute_spv_bytes_written", state.extra_cs_bytes},
	    {"failure_count", state.capture_failures}, {"failures", state.failure_records},
	    {"failures_not_listed", state.capture_failures - state.failure_records.size()},
	    {"scope", "bounded shader capture bookkeeping; required-file manifest determines export completeness"},
	}.dump();
	WriteFile(Root() / "queue-state" / "ds_prelight_shader_status.json", text.data(), text.size());
}

void CaptureFailure(State& state, const std::string& file, uint64_t bytes, const char* reason) {
	++state.capture_failures;
	if (state.failure_records.size() < 256) {
		state.failure_records.push_back({{"file", file}, {"bytes", bytes}, {"reason", reason}});
	}
	WriteCaptureStatus(state);
}

// Reports the first dump failure once instead of failing every pipeline silently.
void ReportFailure(State& state, const std::string& what) {
	if (!state.failed) {
		state.failed = true;
		Log::WriteToConsoleAndLog(fmt::format("WARNING: --dump-shaders: {}\n", what));
	}
}

void WriteDeviceInfo(State& state, const GraphicContext& graphics) {
	if (state.device_written) {
		return;
	}
	state.device_written = true;
	vk::PhysicalDeviceDriverProperties driver {};
	vk::PhysicalDeviceProperties2      properties {};
	properties.pNext = &driver;
	graphics.physical_device.getProperties2(&properties);
	const auto&          p = properties.properties;
	const nlohmann::json info {
	    {"device_name", p.deviceName.data()},
	    {"vendor_id", p.vendorID},
	    {"device_id", p.deviceID},
	    {"driver_version", p.driverVersion},
	    {"api_version", p.apiVersion},
	    {"driver_id", static_cast<uint32_t>(driver.driverID)},
	    {"driver_name", driver.driverName.data()},
	    {"driver_info", driver.driverInfo.data()},
	};
	const auto text = info.dump(1);
	if (!WriteFile(Root() / "device.json", text.data(), text.size())) {
		ReportFailure(state, "cannot write device.json");
	}
}

std::string WriteRecord(State& state, const std::string& prefix, const nlohmann::json& record) {
	const auto text = record.dump(1);
	const auto name = fmt::format("{}_{:016x}.json", prefix, XXH3_64bits(text.data(), text.size()));
	const auto path = Root() / "pipelines" / name;
	if (!ExistingFileMatches(path, text.data(), text.size())) {
		if (DsDecisionCapture() && text.size() > 32ull * 1024 * 1024 - state.decision_pipeline_bytes) {
			CaptureFailure(state, "pipelines/" + name, text.size(), "pipeline_json_budget_32MiB");
			return name;
		}
		if (!WriteFile(path, text.data(), text.size())) {
			if (DsDecisionCapture()) CaptureFailure(state, "pipelines/" + name, text.size(), "pipeline_json_write_failed");
			ReportFailure(state, fmt::format("cannot write {}", name));
		} else if (DsDecisionCapture()) {
			state.decision_pipeline_bytes += text.size();
		}
	}
	return name;
}

std::string WriteModule(State& state, const char* stage_name, uint64_t guest_hash,
                        std::span<const uint32_t> spirv) {
	const auto content = XXH3_64bits(spirv.data(), spirv.size_bytes());
	const auto file = fmt::format("spv/{}_{:016x}_{:016x}.spv", stage_name, guest_hash, content);
	if (ExistingFileMatches(Root() / file, spirv.data(), spirv.size_bytes())) return file;
	const bool decision = DsDecisionCapture();
	const bool graphics = std::string_view(stage_name) != "cs";
	const bool extra = DsLightingCapture() && !graphics && !DsLightingShader(guest_hash);
	if (extra || (decision && graphics)) {
		auto& bytes = graphics ? state.decision_graphics_bytes : state.extra_cs_bytes;
		if (spirv.size_bytes() > 96ull * 1024 * 1024 - bytes) {
			if (decision) CaptureFailure(state, file, spirv.size_bytes(),
			    graphics ? "graphics_spv_budget_96MiB" : "compute_spv_budget_96MiB");
			return file;
		}
	}
	if (!WriteFile(Root() / file, spirv.data(), spirv.size_bytes())) {
		if (decision) CaptureFailure(state, file, spirv.size_bytes(), "native_module_write_failed");
		ReportFailure(state, fmt::format("cannot write {}", file));
		return file;
	}
	if (extra) state.extra_cs_bytes += spirv.size_bytes();
	if (decision && graphics) state.decision_graphics_bytes += spirv.size_bytes();
	return file;
}

PipelineRecord::ModuleNamer Namer(State& state) {
	return [&state](vk::ShaderModule module) -> PipelineRecord::ModuleName {
		const auto it = state.modules.find(static_cast<VkShaderModule>(module));
		if (it == state.modules.end()) {
			return {};
		}
		return {.file = it->second.file, .guest_hash = it->second.guest_hash};
	};
}

} // namespace

bool Enabled() {
	return !Root().empty();
}

bool DsLightingCapture() {
	return Root().filename() == "ds-lighting-dumps";
}

bool DsDecisionCapture() {
	return DsLightingCapture() && DemonsCausalMatrix::Mode() == DemonsCausalMatrix::Trace;
}

void RecordDsProbeGuestCode(uint64_t hash, std::span<const uint32_t> code, const char* stage_name) {
	if (!Enabled()) return;
	const auto relative = fmt::format("guest-shaders/{}_{:016x}_probe.bin", stage_name, hash);
	auto& state = GetState();
	std::scoped_lock lock(state.mutex);
	if (state.ds_guest_attempted.contains(relative)) return;
	state.ds_guest_attempted.insert(relative);
	const bool decision = DsDecisionCapture();
	if (code.empty() || code.size_bytes() > 1024u * 1024u) {
		if (decision) CaptureFailure(state, relative, code.size_bytes(), "guest_code_empty_or_over_1MiB");
		return;
	}
	if ((!decision && state.ds_probe_guest_code.size() >= 96u) ||
	    (decision && code.size_bytes() > 32ull * 1024 * 1024 - state.decision_guest_bytes)) {
		if (decision) CaptureFailure(state, relative, code.size_bytes(), "guest_code_budget_32MiB");
		return;
	}
	if (WriteFile(Root() / relative, code.data(), code.size_bytes())) {
		state.ds_probe_guest_code.insert(relative);
		if (decision) state.decision_guest_bytes += code.size_bytes();
		if (state.ds_probe_guest_code.size() == 1 || state.ds_probe_guest_code.size() % 16 == 0)
			WriteCaptureStatus(state);
	} else {
		if (decision) CaptureFailure(state, relative, code.size_bytes(), "guest_code_write_failed");
		ReportFailure(state, fmt::format("cannot write {}", relative));
	}
}

void RecordModule(vk::ShaderModule module, const char* stage_name, uint64_t guest_hash,
                  std::span<const uint32_t> spirv) {
	if (!Enabled() || module == nullptr ||
	    (DsLightingCapture() && !DsDecisionCapture() && !DsLightingShader(guest_hash) &&
	     !DemonsDominantGeometry::Watched(guest_hash) &&
	     std::ranges::find(DemonsQueueProbe::PostShaders, guest_hash) ==
	         DemonsQueueProbe::PostShaders.end() &&
	     std::ranges::find(DemonsQueueProbe::GBufferShaders, guest_hash) ==
	         DemonsQueueProbe::GBufferShaders.end())) {
		return;
	}
	auto& state = GetState();
	std::scoped_lock lock(state.mutex);
	const auto file = WriteModule(state, stage_name, guest_hash, spirv);
	// Retain the identity even on failure so a required dependency cannot
	// disappear as an empty filename in the witness or export inventory.
	state.modules[static_cast<VkShaderModule>(module)] = {.file = file, .guest_hash = guest_hash};
}

std::string RecordUninstrumentedModule(const char* stage_name, uint64_t guest_hash,
                                      std::span<const uint32_t> spirv) {
	if (!Enabled() || !DsDecisionCapture()) return {};
	auto& state = GetState();
	std::scoped_lock lock(state.mutex);
	return WriteModule(state, stage_name, guest_hash, spirv);
}

std::string ModuleFile(vk::ShaderModule module) {
    if(!Enabled() || !module)return {};
    auto& state=GetState();
    std::scoped_lock lock(state.mutex);
    const auto it=state.modules.find(static_cast<VkShaderModule>(module));
    return it==state.modules.end()?std::string{}:it->second.file;
}

void ForgetModule(vk::ShaderModule module) {
	if (!Enabled()) {
		return;
	}
	auto&            state = GetState();
	std::scoped_lock lock(state.mutex);
	state.modules.erase(static_cast<VkShaderModule>(module));
}

std::string RecordComputePipeline(const GraphicContext& graphics,
                                  const vk::ComputePipelineCreateInfo& info,
                                  const LayoutState& layout, uint64_t guest_hash) {
	if (!Enabled() || (DsLightingCapture() && !DsDecisionCapture() && !DsLightingShader(guest_hash))) {
		return {};
	}
	auto&            state = GetState();
	std::scoped_lock lock(state.mutex);
	WriteDeviceInfo(state, graphics);
	std::string error;
	const auto  record = PipelineRecord::EncodeCompute(
        info, layout, Namer(state), &error);
	if (!record) {
		ReportFailure(state, fmt::format("compute pipeline 0x{:016x} not recorded: {}",
		                                 guest_hash, error));
		return {};
	}
	return WriteRecord(state, fmt::format("cs_{:016x}", guest_hash), *record);
}

std::string RecordGraphicsPipeline(const GraphicContext& graphics,
                                   const vk::GraphicsPipelineCreateInfo& info,
                                   const LayoutState& layout,
                                   std::span<const uint64_t> guest_hashes) {
	// Graphics pipeline records are normally omitted from this focused dump,
	// but the captured light-atlas storage writer is a graphics shader, not
	// necessarily a compute dispatch. Include only its pipelines so we can
	// verify the executable stage, descriptor set and guest hash.
	if (!Enabled() || (DsLightingCapture() && !DsDecisionCapture() &&
	    std::ranges::find(guest_hashes, 0xc03afccb96a13c5dull) == guest_hashes.end() &&
	    !std::ranges::any_of(guest_hashes, DemonsDominantGeometry::Watched))) {
		return {};
	}
	auto&            state = GetState();
	std::scoped_lock lock(state.mutex);
	WriteDeviceInfo(state, graphics);
	std::string prefix = "gfx";
	for (const auto hash: guest_hashes) {
		prefix += fmt::format("_{:016x}", hash);
	}
	std::string error;
	const auto  record = PipelineRecord::EncodeGraphics(
        info, layout, Namer(state), &error);
	if (!record) {
		ReportFailure(state, fmt::format("graphics pipeline {} not recorded: {}", prefix, error));
		return {};
	}
	return WriteRecord(state, prefix, *record);
}

void RecordResult(const std::string& record, vk::Result result) {
	if (!Enabled() || record.empty()) {
		return;
	}
	auto&            state = GetState();
	std::scoped_lock lock(state.mutex);
	std::ofstream    log(Root() / "pipelines.log", std::ios::app);
	log << record << ' ' << vk::to_string(result) << '\n';
}

} // namespace Libs::Graphics::ShaderDump
