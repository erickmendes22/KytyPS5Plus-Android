#include "graphics/host_gpu/renderer/demonsQueueProbe.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
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
	std::unordered_set<uint64_t>                   ds_probe_guest_code;
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
	static constexpr std::array<uint64_t, 11> watched {{
	    0x2f493aadc660cf03ull, 0x567d21ad45386c8dull,
	    0xc03afccb96a13c5dull, 0x3dfe402653925fbfull, 0x6cbfe18ea2005263ull,
	    0x1ce1a858f62b5a11ull, 0xc2e01fbe98f5ebabull,
	    0x23056085c1120461ull, 0xfabe0bcb275a3005ull,
	    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
	}};
	return hash == DemonsQueueProbe::TileProducer ||
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
	return static_cast<bool>(file);
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
	std::error_code ec;
	if (!std::filesystem::exists(path, ec) && !WriteFile(path, text.data(), text.size())) {
		ReportFailure(state, fmt::format("cannot write {}", name));
	}
	return name;
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

void RecordDsProbeGuestCode(uint64_t hash, std::span<const uint32_t> code) {
	if (code.empty() || code.size_bytes() > 1024u * 1024u) return;
	auto& state = GetState();
	std::scoped_lock lock(state.mutex);
	if (state.ds_probe_guest_code.size() >= 96 || state.ds_probe_guest_code.contains(hash)) return;
	const auto file = Root() / "guest-shaders" / fmt::format("cs_{:016x}_probe.bin", hash);
	if (WriteFile(file, code.data(), code.size_bytes())) state.ds_probe_guest_code.insert(hash);
	else ReportFailure(state, fmt::format("cannot write {}", file.filename().string()));
}

void RecordModule(vk::ShaderModule module, const char* stage_name, uint64_t guest_hash,
                  std::span<const uint32_t> spirv) {
	if (!Enabled() || module == nullptr ||
	    (DsLightingCapture() && !DsLightingShader(guest_hash) &&
	     std::ranges::find(DemonsQueueProbe::PostShaders, guest_hash) ==
	         DemonsQueueProbe::PostShaders.end() &&
	     std::ranges::find(DemonsQueueProbe::GBufferShaders, guest_hash) ==
	         DemonsQueueProbe::GBufferShaders.end())) {
		return;
	}
	auto&      state   = GetState();
	const auto content = XXH3_64bits(spirv.data(), spirv.size_bytes());
	const auto file    = fmt::format("spv/{}_{:016x}_{:016x}.spv", stage_name, guest_hash, content);
	std::scoped_lock lock(state.mutex);
	std::error_code  ec;
	if (!std::filesystem::exists(Root() / file, ec) &&
	    !WriteFile(Root() / file, spirv.data(), spirv.size_bytes())) {
		ReportFailure(state, fmt::format("cannot write {}", file));
		return;
	}
	state.modules[static_cast<VkShaderModule>(module)] = {.file = file, .guest_hash = guest_hash};
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
	if (!Enabled() || (DsLightingCapture() && !DsLightingShader(guest_hash))) {
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
	if (!Enabled() || (DsLightingCapture() &&
	    std::ranges::find(guest_hashes, 0xc03afccb96a13c5dull) == guest_hashes.end())) {
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
