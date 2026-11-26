#pragma once
#include "common/emulatorConfig.h"
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
        {"target_frame",target.load()},{"scope","native same-stream differential; not a guest correctness certificate"}};
}
inline bool Configure(bool present, bool markers) {
#ifdef __ANDROID__
    if(!Requested())return false;
    std::error_code ec;
    // Old captures are never allowed to masquerade as this session's reference.
    std::filesystem::remove_all(Folder(),ec);
    if(ec){Status("cleanup_failed");return false;}
    std::filesystem::create_directories(Folder(),ec);
    if(ec||!present||!markers){Status(!present?"capture_layer_unavailable":"debug_markers_unavailable");return false;}
    const auto filename=std::filesystem::absolute(Folder()/"witness.gfxr").string();
    if(setenv("debug.gfxrecon.capture_file",filename.c_str(),1)||
       setenv("debug.gfxrecon.capture_file_timestamp","false",1)||
       setenv("debug.gfxrecon.capture_android_trigger","false",1)||
       setenv("debug.gfxrecon.capture_trigger_boundary","queue_submits",1)||
       setenv("debug.gfxrecon.capture_trigger_frames","2048",1)||
       setenv("debug.gfxrecon.memory_tracking_mode","unassisted",1)||
       setenv("debug.gfxrecon.capture_file_flush","true",1)){
        Status("capture_configuration_failed");return false;
    }
    available.store(true);
    Status("armed");
    return true;
#else
    (void)present;(void)markers;return false;
#endif
}
inline void Observe(int32_t frame,double luminance,uint32_t lit) {
#ifdef __ANDROID__
    if(!available.load())return;
    const int32_t f=target.load();
    if(f>=0){
        if(frame>=f+2&&!stopped.exchange(true)){
            if(setenv("debug.gfxrecon.capture_android_trigger","false",1))
                Status("stop_trigger_failed",frame);
            else Status("stop_requested",frame);
        }
        return;
    }
    // Select on the actual post-processing input, not the earlier
    // layer-zero collapse or the transparent UI. Candidate only.
    // The frame floor avoids the initial boot fade; it is not a proof of a defect.
    if(frame<240||lit>8||luminance>=0.003)return;
    int32_t expected=-1;
    if(!target.compare_exchange_strong(expected,frame+1))return;
    if(setenv("debug.gfxrecon.capture_android_trigger","true",1)){
        stopped.store(true);Status("start_trigger_failed",frame);return;
    }
    Status("start_requested",frame);
#else
    (void)frame;(void)luminance;(void)lit;
#endif
}
inline bool FullFrame(int32_t frame){return available.load()&&!stopped.load()&&target.load()==frame;}
inline bool FlipFrame(int32_t frame){const auto f=target.load();return available.load()&&f>=0&&frame>=f&&frame<=f+1;}
}
