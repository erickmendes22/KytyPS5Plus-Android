#include "tools/shader_checker/checkerPipeline.h"

#include "graphics/host_gpu/renderer/pipeline/pipelineRecord.h"

#include <chrono>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <optional>
#include <spirv-tools/libspirv.hpp>
#include <vector>

namespace ShaderChecker {

namespace {

using Libs::Graphics::PipelineRecord::Pipeline;

std::optional<std::vector<uint32_t>> ReadSpirv(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
	                              std::istreambuf_iterator<char>());
	if (bytes.empty() || bytes.size() % 4 != 0) {
		return std::nullopt;
	}
	std::vector<uint32_t> words(bytes.size() / 4);
	std::memcpy(words.data(), bytes.data(), bytes.size());
	return words;
}

std::optional<nlohmann::json> ReadJson(const std::filesystem::path& path) {
	std::ifstream file(path);
	if (!file) {
		return std::nullopt;
	}
	auto json = nlohmann::json::parse(file, nullptr, false);
	if (json.is_discarded()) {
		return std::nullopt;
	}
	return json;
}

std::string Validate(const std::vector<uint32_t>& words) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer(
	    [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
		    if (messages.empty()) messages = message;
	    });
	spvtools::ValidatorOptions options;
	options.SetAllowOffsetTextureOperand(true);
	return tools.Validate(words.data(), words.size(), options) ? std::string() : messages;
}

// Owns every Vulkan object of one check so all exits release them.
struct Objects {
	explicit Objects(vk::Device device): device(device) {}
	~Objects() {
		if (pipeline != nullptr) device.destroyPipeline(pipeline);
		if (layout != nullptr) device.destroyPipelineLayout(layout);
		if (set_layout != nullptr) device.destroyDescriptorSetLayout(set_layout);
		for (const auto module: modules) device.destroyShaderModule(module);
	}
	Objects(const Objects&)            = delete;
	Objects& operator=(const Objects&) = delete;

	vk::Device                    device;
	std::vector<vk::ShaderModule> modules;
	vk::DescriptorSetLayout       set_layout = nullptr;
	vk::PipelineLayout            layout     = nullptr;
	vk::Pipeline                  pipeline   = nullptr;
};

CheckResult Skip(CheckResult result, std::string reason) {
	result.status = Status::Skip;
	result.reason = std::move(reason);
	return result;
}

CheckResult Failed(CheckResult result, std::string reason) {
	result.status = Status::Fail;
	result.reason = std::move(reason);
	return result;
}

bool CreateModules(const Device& device, Pipeline& pipeline, const CheckOptions& options,
                   Objects& objects, CheckResult& result) {
	for (size_t i = 0; i < pipeline.Stages().size(); i++) {
		const auto& stage = pipeline.Stages()[i];
		result.guests += fmt::format("{}{:016x}", result.guests.empty() ? "" : " ", stage.guest_hash);
		const auto replacement = options.replacements.find(i);
		const auto path        = replacement != options.replacements.end()
		                             ? std::filesystem::path(replacement->second)
		                             : options.root / stage.spv;
		const auto words       = ReadSpirv(path);
		if (!words) {
			result = Skip(result, "cannot read " + path.string());
			return false;
		}
		result.spirv_words += words->size();
		if (options.validate) {
			if (auto message = Validate(*words); !message.empty()) {
				result = Failed(result, "spirv-val: " + message);
				return false;
			}
		}
		vk::ShaderModuleCreateInfo info {};
		info.codeSize = words->size() * sizeof(uint32_t);
		info.pCode    = words->data();
		vk::ShaderModule module = nullptr;
		if (const auto r = device.Handle().createShaderModule(&info, nullptr, &module);
		    r != vk::Result::eSuccess) {
			result = Failed(result, "vkCreateShaderModule " + vk::to_string(r));
			return false;
		}
		objects.modules.push_back(module);
	}
	return true;
}

} // namespace

CheckResult CheckRecord(const Device& device, const std::filesystem::path& record,
                        const CheckOptions& options) {
	CheckResult result;
	const auto  json = ReadJson(record);
	if (!json) {
		return Skip(result, "unreadable record");
	}
	std::string error;
	auto        pipeline = Pipeline::Decode(*json, &error);
	if (!pipeline) {
		return Skip(result, error);
	}
	Objects objects(device.Handle());
	if (!CreateModules(device, *pipeline, options, objects, result)) {
		return result;
	}
	const auto layout_info = pipeline->LayoutInfo();
	if (const auto r = device.Handle().createDescriptorSetLayout(&layout_info, nullptr,
	                                                              &objects.set_layout);
	    r != vk::Result::eSuccess) {
		return Skip(result, "vkCreateDescriptorSetLayout " + vk::to_string(r));
	}
	vk::PipelineLayoutCreateInfo layout {};
	layout.setLayoutCount         = 1;
	layout.pSetLayouts            = &objects.set_layout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges    = &pipeline->PushConstants();
	if (const auto r = device.Handle().createPipelineLayout(&layout, nullptr, &objects.layout);
	    r != vk::Result::eSuccess) {
		return Skip(result, "vkCreatePipelineLayout " + vk::to_string(r));
	}

	const auto begin = std::chrono::steady_clock::now();
	vk::Result r;
	if (pipeline->IsCompute()) {
		const auto info = pipeline->BuildCompute(objects.modules.front(), objects.layout);
		r = device.Handle().createComputePipelines(nullptr, 1, &info, nullptr, &objects.pipeline);
	} else {
		const auto info = pipeline->BuildGraphics(objects.modules, objects.layout);
		r = device.Handle().createGraphicsPipelines(nullptr, 1, &info, nullptr, &objects.pipeline);
	}
	result.milliseconds = std::chrono::duration<double, std::milli>(
	                          std::chrono::steady_clock::now() - begin)
	                          .count();
	result.status = r == vk::Result::eSuccess ? Status::Ok : Status::Fail;
	result.reason = vk::to_string(r);
	return result;
}

} // namespace ShaderChecker
