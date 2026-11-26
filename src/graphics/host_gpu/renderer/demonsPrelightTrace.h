#pragma once
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include "graphics/host_gpu/renderer/demonsQueueProbe.h"
#include "graphics/host_gpu/renderer/demonsPrelightPolicy.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "common/emulatorConfig.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <algorithm>

namespace Libs::Graphics {
class RenderContext;
struct PreparedBindings;
namespace DemonsPrelightTrace {
inline bool Enabled() {
    return DemonsCausalMatrix::Mode()==DemonsCausalMatrix::Trace &&
        Config::GetShaderDumpFolder().filename()=="ds-lighting-dumps";
}
inline std::atomic<uint64_t> known_size{1};
inline bool Matches(uint64_t address,uint64_t bytes) {
    return DemonsPrelightPolicy::Overlaps(address,bytes,DemonsQueueProbe::GBufferAddress,
        known_size.load(std::memory_order_relaxed));
}
inline void Watch(uint64_t address,uint64_t bytes){
    if(address!=DemonsQueueProbe::GBufferAddress||!bytes)return;
    auto old=known_size.load(std::memory_order_relaxed);
    while(old<bytes&&!known_size.compare_exchange_weak(old,bytes,std::memory_order_relaxed)){}
}
inline nlohmann::json Identity(const Image& image) {
    return {{"owner",image.info.data.address},{"bytes",image.info.data.size},
        {"object",reinterpret_cast<uintptr_t>(&image)},
        {"native",std::hash<VkImage>{}(static_cast<VkImage>(image.backing.image))},
        {"version",image.ContentVersion()},{"write_order",image.gpu_write_order},
        {"format",uint32_t(image.backing.format)},{"width",image.backing.extent.width},
        {"height",image.backing.extent.height},{"gpu",image.IsGpuModified()},
        {"cpu_dirty",image.IsCpuDirty()},{"buffer_modified",image.IsBufferModified()}};
}
struct Journal {
    std::mutex mutex;
    uint64_t seq=0,dropped=0,samples=0,sample_dropped=0,full=0,full_dropped=0;
    size_t bytes=0;
    unsigned chunk=0;
    std::ofstream out;
};
inline Journal& State(){static Journal j;return j;}
inline constexpr size_t ChunkBytes=2u*1024u*1024u;
inline constexpr unsigned MaxChunks=16;
inline thread_local uint64_t active_draw=0;
inline std::filesystem::path Folder(){return Config::GetShaderDumpFolder()/"queue-state";}
inline void StatusLocked(const Journal& j) {
    std::ofstream out(Folder()/"ds_prelight_status.json",std::ios::trunc);
    if(out)out<<nlohmann::json{{"schema",1},{"node","N00"},{"last_seq",j.seq},
        {"dropped_events",j.dropped},{"samples",j.samples},{"sample_dropped",j.sample_dropped},
        {"full_samples",j.full},{"full_dropped",j.full_dropped},{"chunks",j.chunk+(j.out.is_open()?1u:0u)},
        {"scope","observed target range; candidate cache events, not complete guest oracle"},
        {"root_decision","blocked_until_expected_word_and_complete_same_cycle_history"}};
}
inline uint64_t Record(nlohmann::json event) {
    if(!Enabled())return 0;
    auto& j=State();std::scoped_lock lock(j.mutex);
    const auto seq=++j.seq;event["seq"]=seq;
    auto line=event.dump();
    if(!j.out.is_open()||j.bytes+line.size()+1>ChunkBytes){
        if(j.out.is_open()){j.out.close();++j.chunk;}
        if(j.chunk>=MaxChunks){++j.dropped;if(j.dropped==1||j.dropped%256==0)StatusLocked(j);return 0;}
        std::error_code ec;std::filesystem::create_directories(Folder(),ec);
        j.out.open(Folder()/("ds_prelight_events_"+std::to_string(j.chunk)+".jsonl"),std::ios::trunc);
        j.bytes=0;
    }
    if(!j.out){++j.dropped;StatusLocked(j);return 0;}
    j.out<<line<<'\n';j.out.flush();
    if(!j.out){++j.dropped;StatusLocked(j);return 0;}
    j.bytes+=line.size()+1;
    if(seq==1||seq%128==0||event.value("kind",std::string{})=="consumer")StatusLocked(j);
    return seq;
}
inline bool AllowSample(bool full=false){
    if(!Enabled())return false;
    auto& j=State();std::scoped_lock lock(j.mutex);
    auto& used=full?j.full:j.samples;auto& dropped=full?j.full_dropped:j.sample_dropped;
    if(j.chunk>=MaxChunks||used>=(full?2400u:48000u)){
        ++dropped;if(dropped==1||dropped%256==0)StatusLocked(j);return false;
    }
    ++used;return true;
}
inline void Cache(uint64_t tick,const Image& image,const char* operation,
                  uint64_t source=0,uint64_t detail=0) {
    if(!Enabled()||!Matches(image.info.data.address,image.info.data.size))return;
    Watch(image.info.data.address,image.info.data.size);
    // A claim precedes execution; actual boundary records retain version/order.
    if(std::string_view(operation)=="commit_gpu_write_claim")return;
    Record({{"kind","cache"},{"operation",operation},{"tick",tick},
        {"image",Identity(image)},{"source",source},{"detail",detail}});
}
inline void Range(uint64_t tick,const char* operation,uint64_t address,uint64_t bytes,uint64_t detail=0){
    if(!Enabled()||!Matches(address,bytes))return;
    Record({{"kind","range"},{"operation",operation},{"tick",tick},
        {"address",address},{"bytes",bytes},{"detail",detail}});
}
inline void BufferWriter(uint64_t tick,const char* op,uint64_t address,uint64_t bytes,
        uint64_t operand,const char* route,bool overflow=false){
    if(!Enabled()||!Matches(address,bytes))return;
    Record({{"kind","buffer_writer"},{"operation",op},{"tick",tick},
        {"address",address},{"bytes",bytes},{"operand",operand},{"route",route},{"overflow",overflow}});
}
void ObserveCompute(RenderContext& context,const PreparedBindings& bindings,uint64_t submit,
                    bool after,bool indirect,uint64_t args_address=0);
}
}
