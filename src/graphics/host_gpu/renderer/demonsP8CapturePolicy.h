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
// #750: full-screen direct HDR contribution occurs after deferred q33.
constexpr uint64_t DirectScenePS=0xab81bfa8ee4e0feeull;
constexpr uint64_t HdrOwner=0x281300000ull;
constexpr uint64_t SceneOwner=0x2b2a40000ull;
constexpr uint64_t CompositePS=0x3265b01ed00a5c01ull;
constexpr bool DirectSceneTarget(uint64_t shader,uint64_t owner) {
    return shader==DirectScenePS&&owner==HdrOwner;
}
constexpr const char* RadianceInputRole(uint64_t shader,uint64_t owner,
                                       bool gpu,bool read,bool written,
                                       uint32_t levels,uint32_t layers,uint64_t bytes) {
    if(!read||written||!owner||!levels||!layers)return nullptr;
    if((shader==0xe86bfee195efa5a4ull||shader==0x8f52cca9ea3ba4fcull)
        &&owner==HdrOwner&&layers==1)return "post_hdr_input";
    if(shader==CompositePS&&owner==SceneOwner&&layers==1)return "compositor_scene_input";
    // Preserve the dynamic, small 2D inputs of the actual forward pass,
    // not all static BC textures or hundreds of shadow-array layers.
    if(shader==DirectScenePS&&gpu&&layers==1&&levels<=9&&bytes<=8ull*1024*1024)
        return "forward_image_input";
    return nullptr;
}
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
