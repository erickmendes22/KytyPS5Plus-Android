#pragma once
// The PC Wolverine fork uses descriptor indexing + update-after-bind for its
// dynamic guest image heaps. Vulkan 1.2 support must be checked per selected
// Android driver; exposing an extension name alone is insufficient.
#include <cstdint>

namespace Libs::Graphics {
struct BindlessCapability {
    bool runtime_array = false;
    bool non_uniform_sampled_image_indexing = false;
    bool partially_bound = false;
    bool sampled_image_update_after_bind = false;
    bool update_unused_while_pending = false;
    uint32_t update_after_bind_sampled_limit = 0;
    uint32_t update_after_bind_sampler_limit = 0;
    [[nodiscard]] constexpr bool Usable() const noexcept {
        return runtime_array && non_uniform_sampled_image_indexing && partially_bound &&
               sampled_image_update_after_bind && update_unused_while_pending &&
               update_after_bind_sampled_limit >= 64;
    }
};
} // namespace Libs::Graphics
