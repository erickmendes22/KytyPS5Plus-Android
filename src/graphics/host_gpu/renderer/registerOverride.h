#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_REGISTEROVERRIDE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_REGISTEROVERRIDE_H_

// KYTY_GPU_PIPELINE: while a record-thread job runs, CommandBuffer's register accessors return its
// item's snapshot (gpuPipelineItems.h) instead of the live blocks the front keeps writing. The
// override is per thread, so the front's accessors keep returning the live blocks.
namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

template <typename Context, typename UserConfig, typename Shader>
struct BasicRegisterOverride {
	Context*    context     = nullptr;
	UserConfig* user_config = nullptr;
	Shader*     shaders     = nullptr;
};

template <typename T>
[[nodiscard]] inline T& PickRegisters(T* override_block, T& live) noexcept {
	return override_block != nullptr ? *override_block : live;
}

// Installs an override for the current thread and restores the previous one (nestable).
template <typename Override>
class BasicOverrideScope {
public:
	BasicOverrideScope(Override& slot, const Override& value): m_slot(slot), m_previous(slot) {
		m_slot = value;
	}
	~BasicOverrideScope() { m_slot = m_previous; }
	BasicOverrideScope(const BasicOverrideScope&)            = delete;
	BasicOverrideScope& operator=(const BasicOverrideScope&) = delete;

private:
	Override& m_slot;
	Override  m_previous;
};

using RegisterOverride = BasicRegisterOverride<HW::Context, HW::UserConfig, HW::Shader>;
inline thread_local RegisterOverride t_register_override {};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_REGISTEROVERRIDE_H_
