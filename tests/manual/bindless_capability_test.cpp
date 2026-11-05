#include "graphics/host_gpu/renderer/pipeline/bindlessCapability.h"
#include <cassert>
#include <cstdio>
using Libs::Graphics::BindlessCapability;
int main() {
    constexpr BindlessCapability turnip{true,true,true,true,true,65536,4096};
    constexpr BindlessCapability no_runtime{false,true,true,true,true,65536,4096};
    constexpr BindlessCapability no_update{true,true,true,false,true,65536,4096};
    constexpr BindlessCapability no_pending{true,true,true,true,false,65536,4096};
    constexpr BindlessCapability small{true,true,true,true,true,32,4096};
    static_assert(turnip.Usable());
    static_assert(!no_runtime.Usable());
    static_assert(!no_update.Usable());
    static_assert(!no_pending.Usable());
    static_assert(!small.Usable());
    std::puts("Bindless Vulkan capability predicate: OK");
}
