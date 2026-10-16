#include "graphics/shader/deviceQuirks.h"

#include <array>

namespace Libs::Graphics {

namespace {

// VkDriverId values (vulkan_core.h), kept here so the shader layer does not include Vulkan.
constexpr uint32_t kDriverIdQualcommProprietary = 8;

struct QuirkRule {
	const char* name;
	bool ShaderDeviceQuirks::*flag;
	bool (*applies)(const ShaderDriverInfo& driver);
};

bool IsQualcommProprietary(const ShaderDriverInfo& driver) {
	return driver.driver_id == kDriverIdQualcommProprietary;
}

constexpr std::array kRules = {
    QuirkRule {"branchless_bounds_checks", &ShaderDeviceQuirks::branchless_bounds_checks,
               IsQualcommProprietary},
};

const QuirkRule* FindRule(std::string_view name) {
	for (const auto& rule: kRules) {
		if (name == rule.name) {
			return &rule;
		}
	}
	return nullptr;
}

} // namespace

ShaderDeviceQuirks DetectShaderDeviceQuirks(const ShaderDriverInfo& driver) {
	ShaderDeviceQuirks quirks;
	for (const auto& rule: kRules) {
		quirks.*rule.flag = rule.applies(driver);
	}
	return quirks;
}

bool ApplyShaderQuirkOverrides(std::string_view spec, ShaderDeviceQuirks& quirks,
                               std::string* error) {
	while (!spec.empty()) {
		const auto comma = spec.find(',');
		auto       item  = spec.substr(0, comma);
		spec             = comma == std::string_view::npos ? std::string_view {} : spec.substr(comma + 1);
		if (item.empty()) {
			continue;
		}
		if (item == "none") {
			quirks = {};
			continue;
		}
		const bool enable = item.front() != '-';
		if (!enable) {
			item.remove_prefix(1);
		}
		const auto* rule = FindRule(item);
		if (rule == nullptr) {
			if (error != nullptr) {
				*error = "unknown shader quirk: " + std::string(item);
			}
			return false;
		}
		quirks.*rule->flag = enable;
	}
	return true;
}

std::string DescribeShaderQuirks(const ShaderDeviceQuirks& quirks) {
	std::string text;
	for (const auto& rule: kRules) {
		if (quirks.*rule.flag) {
			text += text.empty() ? "" : ",";
			text += rule.name;
		}
	}
	return text.empty() ? "none" : text;
}

} // namespace Libs::Graphics
