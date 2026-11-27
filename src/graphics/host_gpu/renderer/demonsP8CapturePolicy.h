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
// Require separate low observations rather than latch the first fade. Reserve
// the preceding complete guest frame: in #749 all G-buffer writers in frame
// 335 ran AFTER the consumers of version 33155.
struct SustainedDark {
    int32_t last=-1;
    uint32_t low=0;
    constexpr bool Observe(int32_t frame,double luminance,uint32_t lit) {
        if(frame==last)return false;
        if(frame<last||int64_t{frame}-last>48)low=0;
        last=frame;
        if(!Trigger(frame,luminance,lit)){low=0;return false;}
        if(low<3)++low;
        return low==3;
    }
};
constexpr bool ProducerFrame(int32_t frame,int32_t target,bool boundary,bool stopped) {
    return !stopped&&!boundary&&target>0&&frame>=target-1&&frame<=target;
}
constexpr bool Reusable(bool cpu_dirty,bool buffer_modified,bool same_identity,
                        bool same_version,bool same_write_order) {
    return !cpu_dirty&&!buffer_modified&&same_identity&&same_version&&same_write_order;
}
}
