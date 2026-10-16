#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"

#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
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

void RecordModule(vk::ShaderModule module, const char* stage_name, uint64_t guest_hash,
                  std::span<const uint32_t> spirv) {
	if (!Enabled() || module == nullptr) {
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
	if (!Enabled()) {
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
	if (!Enabled()) {
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
