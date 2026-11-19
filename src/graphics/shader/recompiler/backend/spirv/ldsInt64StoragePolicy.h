#pragma once

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Only supported compute programs using native LDS and 64-bit atomics need a
// fallback on a Vulkan device lacking shaderSharedInt64Atomics.
// StorageBuffer LDS uses the already required shaderBufferInt64Atomics.
[[nodiscard]] constexpr bool ShouldUseStorageLdsForInt64(
    bool enabled_for_title, bool compute, bool uses_lds,
    bool needs_shared_int64, bool device_has_shared_int64,
    bool currently_workgroup) noexcept {
    return enabled_for_title && compute && uses_lds && needs_shared_int64 &&
           !device_has_shared_int64 && currently_workgroup;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
