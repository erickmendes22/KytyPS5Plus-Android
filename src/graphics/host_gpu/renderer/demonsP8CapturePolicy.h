#pragma once
#include <cstdint>
namespace Libs::Graphics::DemonsP8CapturePolicy {
// Full-state capture reached 14.2 GB on #748. A post-submit size check
// cannot interrupt that synchronous operation.
constexpr bool AttachWholeStateLayer(bool requested,bool present,bool markers) {
    (void)requested;(void)present;(void)markers;
    return false;
}
constexpr uint64_t RawStorageBudget=192ull*1024*1024;
constexpr uint64_t RawCopyBudget=256ull*1024*1024;
constexpr bool Trigger(int32_t frame,double luminance,uint32_t lit) {
    return frame>=240 && lit<=8 && luminance>=0.0 && luminance<0.003;
}
constexpr bool Reusable(bool cpu_dirty,bool buffer_modified,bool same_identity,
                        bool same_version,bool same_write_order) {
    return !cpu_dirty&&!buffer_modified&&same_identity&&same_version&&same_write_order;
}
}
