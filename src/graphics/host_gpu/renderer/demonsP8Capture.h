#pragma once
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/demonsP8CapturePolicy.h"
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace Libs::Graphics::DemonsP8Capture {
inline std::atomic<bool> available{false};
inline std::atomic<int32_t> target{-1};
inline std::atomic<bool> stopped{false};
inline std::atomic<bool> evidence_started{false};
inline std::atomic<bool> boundary_reached{false};
// All boundary and foreground recording is serialized by the render context
// mutex. Release/acquire publishes the first real consumer's identity.
inline uint64_t boundary_submit=0,boundary_version=0,boundary_owner=0,boundary_order=0;
inline DemonsP8CapturePolicy::SustainedDark dark_selection;
inline bool EvidenceFrame(int32_t frame){
    const auto f=target.load();
    return available.load()&&!stopped.load()&&f>0&&frame>=f-1&&frame<=f;
}
inline bool BeginEvidence(int32_t frame){
    if(!EvidenceFrame(frame))return false;
    evidence_started.store(true);
    return true;
}
inline bool ProducerFrame(int32_t frame){
    return available.load()&&DemonsP8CapturePolicy::ProducerFrame(frame,target.load(),boundary_reached.load(),stopped.load());
}
inline bool MarkBoundary(int32_t frame,uint32_t queue,uint64_t submit,uint64_t owner,uint64_t version,uint64_t order){
    if(!EvidenceFrame(frame)||frame!=target.load()||queue!=0||boundary_reached.load())return false;
    boundary_submit=submit;boundary_owner=owner;boundary_version=version;boundary_order=order;
    boundary_reached.store(true);
    return true;
}
inline nlohmann::json Boundary(){
    if(!boundary_reached.load())return nullptr;
    return {{"frame",target.load()},{"queue",0},{"submit",boundary_submit},
        {"owner",boundary_owner},{"version",boundary_version},{"write_order",boundary_order}};
}
inline constexpr const char* Layer="VK_LAYER_LUNARG_gfxreconstruct";
inline bool Requested() {
#ifdef __ANDROID__
    return DemonsCausalMatrix::Mode()==DemonsCausalMatrix::Trace &&
        Config::GetShaderDumpFolder().filename()=="ds-lighting-dumps";
#else
    return false;
#endif
}
inline std::filesystem::path Folder(){return Config::GetShaderDumpFolder()/"p8-reference";}
inline void Status(const char* state, int32_t frame=-1) {
    std::error_code ec;
    std::filesystem::create_directories(Folder(),ec);
    if(ec)return;
    std::ofstream out(Folder()/"capture-status.json",std::ios::trunc);
    if(out)out<<nlohmann::json{{"schema",1},{"state",state},{"frame",frame},
        {"target_frame",target.load()},{"producer_first_frame",target.load()>0?target.load()-1:-1},
        {"boundary",Boundary()},{"native_replay",false},{"scope","bounded raw observations; not a native replay or guest correctness certificate"}};
}
inline bool Configure(bool present, bool markers) {
#ifdef __ANDROID__
    if(!Requested())return false;
    std::error_code ec;
    // Old captures are never allowed to masquerade as this session's reference.
    std::filesystem::remove_all(Folder(),ec);
    if(ec){Status("cleanup_failed");return false;}
    std::filesystem::create_directories(Folder(),ec);
    if(ec){Status("capture_directory_failed");return false;}
    // Mode F must not synchronously serialize all live Vulkan resources.
    // Raw acquisition remains independent of the layer and debug labels.
    available.store(true);
    Status("raw_only_armed");
    return DemonsP8CapturePolicy::AttachWholeStateLayer(true,present,markers);
#else
    (void)present;(void)markers;return false;
#endif
}
inline void Observe(int32_t frame,double luminance,uint32_t lit) {
#ifdef __ANDROID__
    if(!available.load())return;
    const int32_t f=target.load();
    if(f>=0){
        if(frame>=f+2&&!stopped.exchange(true))Status("raw_only_complete",frame);
        return;
    }
    // Select on the actual post-processing input, not the earlier
    // layer-zero collapse or the transparent UI. Candidate only.
    // The frame floor avoids the initial boot fade; it is not a proof of a defect.
    if(!dark_selection.Observe(frame,luminance,lit))return;
    int32_t expected=-1;
    if(!target.compare_exchange_strong(expected,frame+2))return;
    Status("raw_only_selected",frame);
#else
    (void)frame;(void)luminance;(void)lit;
#endif
}
inline bool FullFrame(int32_t frame){return available.load()&&!stopped.load()&&target.load()==frame;}
inline bool FlipFrame(int32_t frame){const auto f=target.load();return available.load()&&f>=0&&frame>=f&&frame<=f+1;}
}
