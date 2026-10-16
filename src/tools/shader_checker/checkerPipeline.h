#ifndef KYTY_TOOLS_SHADER_CHECKER_CHECKERPIPELINE_H_
#define KYTY_TOOLS_SHADER_CHECKER_CHECKERPIPELINE_H_

#include "tools/shader_checker/checkerDevice.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace ShaderChecker {

struct CheckOptions {
	std::filesystem::path         root;         // dump root: spv/ paths resolve against it
	std::map<size_t, std::string> replacements; // stage index -> SPIR-V file used instead
	bool                          validate = false;
};

enum class Status { Ok, Fail, Skip };

struct CheckResult {
	Status      status = Status::Skip;
	std::string reason; // VkResult or why the record was skipped
	std::string guests; // guest hashes of the stages, space separated
	double      milliseconds = 0.0;
	size_t      spirv_words  = 0;
};

// Rebuilds the pipeline described by one dump record on `device` and returns how the driver
// answered. Every Vulkan object is destroyed before returning.
CheckResult CheckRecord(const Device& device, const std::filesystem::path& record,
                        const CheckOptions& options);

} // namespace ShaderChecker

#endif /* KYTY_TOOLS_SHADER_CHECKER_CHECKERPIPELINE_H_ */
