#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_DEVICEQUIRKS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_DEVICEQUIRKS_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::Graphics {

// The Vulkan driver shaders are compiled for (VkPhysicalDeviceProperties and
// VkPhysicalDeviceDriverProperties). Plain integers keep this header free of Vulkan.
struct ShaderDriverInfo {
	uint32_t vendor_id      = 0;
	uint32_t device_id      = 0;
	uint32_t driver_id      = 0; // VkDriverId
	uint32_t driver_version = 0;
};

// Workarounds for host shader compilers: one flag per known driver bug or limit, so every driver
// workaround has one home. All flags off is the portable output that desktop drivers get;
// DetectShaderDeviceQuirks turns flags on per driver, and the SPIR-V emitter reads them through
// CompileOptions::device_quirks. A new workaround adds a flag here plus a row in the table in
// deviceQuirks.cpp (name, driver predicate, reason).
struct ShaderDeviceQuirks {
	// Qualcomm's proprietary compiler fails "Failed to link shaders" on shaders with very many
	// bounds-checked buffer loads: each one is emitted as a branch around the load. With this
	// flag the emitter clamps the index and selects zero instead, keeping such code branch-free.
	bool branchless_bounds_checks = false;

	bool operator==(const ShaderDeviceQuirks&) const = default;
};

[[nodiscard]] ShaderDeviceQuirks DetectShaderDeviceQuirks(const ShaderDriverInfo& driver);

// Applies comma-separated overrides on top of detection: "name" enables, "-name" disables, "none"
// clears every flag. Returns false (with *error set) for an unknown name.
[[nodiscard]] bool ApplyShaderQuirkOverrides(std::string_view spec, ShaderDeviceQuirks& quirks,
                                             std::string* error);

// "name,name" of the enabled flags, or "none".
[[nodiscard]] std::string DescribeShaderQuirks(const ShaderDeviceQuirks& quirks);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_DEVICEQUIRKS_H_ */
