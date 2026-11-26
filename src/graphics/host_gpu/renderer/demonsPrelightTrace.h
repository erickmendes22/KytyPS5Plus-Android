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
#include <vector>

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
inline constexpr unsigned MaxChunks=8;
inline thread_local uint64_t active_draw=0;
inline thread_local uint64_t command_seq=0;
inline thread_local bool command_prepared=false;
inline std::mutex phase_mutex;
inline DemonsPrelightPolicy::ProductionWindow production_window;
inline int Phase(int32_t frame){std::scoped_lock lock(phase_mutex);return production_window.Phase(frame);}
inline void ObservePopulation(int32_t frame,uint64_t colored,uint64_t area){
    std::scoped_lock lock(phase_mutex);production_window.Observe(frame,colored,area);
}
struct VertexRange{uint64_t address=0,bytes=0;bool confirmed=false;bool upstream=false;};
// Prior addresses are candidate seeds only; live VS descriptors certify identity.
inline std::mutex vertex_mutex;
inline std::vector<VertexRange> vertex_ranges{{0x20cca0e40ull,31488,false},{0x20cca8940ull,10496,false},{0x20ccab240ull,31488,false}};
inline std::vector<uint64_t> vertex_shaders;
inline std::vector<VertexRange> vertex_inputs;
inline void WatchVertexInput(uint64_t address,uint64_t bytes){
    if(!Enabled()||!address||bytes!=31488)return;
    std::scoped_lock lock(vertex_mutex);
    for(auto& r:vertex_inputs)if(r.address==address)return;
    if(vertex_inputs.size()>=256)return;
    vertex_inputs.push_back({address,bytes,true,true});
}
inline std::vector<VertexRange> VertexInputs(){
    std::scoped_lock lock(vertex_mutex);return vertex_inputs;
}
inline void WatchVertex(uint64_t address,uint64_t bytes){
    if(!Enabled()||!address||!bytes||bytes>65536)return;
    std::scoped_lock lock(vertex_mutex);
    for(auto& x:vertex_ranges)if(x.address==address&&x.bytes==bytes){x.confirmed=true;return;}
    if(vertex_ranges.size()<20)vertex_ranges.push_back({address,bytes,true});
}
inline bool VertexMatches(uint64_t address,uint64_t bytes){
    if(!Enabled())return false;
    std::scoped_lock lock(vertex_mutex);
    for(auto x:vertex_ranges)if(DemonsPrelightPolicy::Overlaps(address,bytes,x.address,x.bytes))return true;
    // Source CPU provenance is recorded separately.
    return false;
}
inline std::vector<VertexRange> VertexRanges(){
    std::scoped_lock lock(vertex_mutex);
    auto ranges=vertex_ranges;
    ranges.insert(ranges.end(),vertex_inputs.begin(),vertex_inputs.end());
    return ranges;
}
inline void WatchVertexShader(uint64_t shader){
    if(!shader)return;
    std::scoped_lock lock(vertex_mutex);
    if(vertex_shaders.size()<16&&std::ranges::find(vertex_shaders,shader)==vertex_shaders.end())vertex_shaders.push_back(shader);
}
inline bool VertexShader(uint64_t shader){
    std::scoped_lock lock(vertex_mutex);
    return std::ranges::find(vertex_shaders,shader)!=vertex_shaders.end();
}
inline bool IsNanWord(uint32_t w){return (w&0x7f800000u)==0x7f800000u&&(w&0x007fffffu)!=0;}
std::vector<uint32_t> ReadNativeWords(RenderContext&,vk::Buffer,uint64_t,uint32_t);
inline std::filesystem::path Folder(){return Config::GetShaderDumpFolder()/"queue-state";}
// Independent bounded timeline: root draw census must not evict the XYZ edge.
inline void VertexRecord(nlohmann::json event) {
    if(!Enabled())return;
    static std::mutex mutex;
    static uint64_t seq=0,bytes=0,dropped=0;
    static std::ofstream out;
    std::scoped_lock lock(mutex);
    event["vertex_seq"]=++seq;
    const auto line=event.dump();
    constexpr uint64_t budget=8u*1024u*1024u;
    if(!out.is_open()&&seq==1){
        std::error_code ec;std::filesystem::create_directories(Folder(),ec);
        out.open(Folder()/"ds_vertex_producer_timeline.jsonl",std::ios::trunc);
    }
    if(!out||bytes+line.size()+1>budget)++dropped;
    else {out<<line<<'\n';out.flush();if(out)bytes+=line.size()+1;else ++dropped;}
    if(seq==1||seq%64==0||dropped){
        std::ofstream status(Folder()/"ds_vertex_producer_timeline_status.json",std::ios::trunc);
        if(status)status<<nlohmann::json{{"events",seq},{"bytes",bytes},{"dropped",dropped},{"scope","candidate writer and exact consumer ranges"}};
    }
}
inline bool VertexConsumerFrame(uint64_t address,int32_t frame){
    static std::mutex mutex;
    static std::vector<std::pair<uint64_t,int32_t>> seen;
    std::scoped_lock lock(mutex);
    for(auto& e:seen)if(e.first==address){if(e.second==frame)return false;e.second=frame;return true;}
    seen.emplace_back(address,frame);return true;
}
inline uint64_t VertexWordDigest(const std::vector<uint32_t>& words){
    uint64_t hash=14695981039346656037ull;
    for(uint32_t w:words){hash^=w;hash*=1099511628211ull;}
    return hash;
}
// Save actual staging bytes' fingerprints before the copy, including aligned
// source candidates that have not yet been discovered by the consumer.
struct VertexUploadSample {
    uint64_t address=0,bytes=0,tick=0,digest=0,nans=0;
};
inline std::mutex vertex_upload_mutex;
inline std::vector<VertexUploadSample> vertex_upload_samples;
inline void NoteVertexUpload(uint64_t tick,uint64_t address,const uint8_t* data,uint64_t bytes){
    if(!Enabled()||!data||!bytes)return;
    auto inputs=VertexInputs();
    // #732 source ranges are 1 MiB aligned in both independent cycles.
    // Treat alignment as a candidate filter only, never as identity.
    constexpr uint64_t alignment=1ull<<20, length=31488;
    const auto first=(address+alignment-1)&~(alignment-1);
    for(uint64_t a=first;a>=address&&a-address<bytes&&a-first<8*alignment;a+=alignment)
        if(bytes-(a-address)>=length)inputs.push_back({a,length,false,true});
    for(const auto& r:inputs){
        if(!DemonsPrelightPolicy::Overlaps(address,bytes,r.address,r.bytes))continue;
        const auto begin=std::max(address,r.address);
        const auto end=std::min(address+bytes,r.address+r.bytes);
        if(begin%4||end%4||end<=begin)continue;
        uint64_t hash=14695981039346656037ull,nans=0;
        for(uint64_t a=begin;a<end;a+=4){
            const auto* p=data+(a-address);
            const uint32_t w=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
            hash^=w;hash*=1099511628211ull;nans+=IsNanWord(w);
        }
        {
            std::scoped_lock lock(vertex_upload_mutex);
            if(vertex_upload_samples.size()>=4096)vertex_upload_samples.erase(vertex_upload_samples.begin(),vertex_upload_samples.begin()+1024);
            vertex_upload_samples.push_back({begin,end-begin,tick,hash,nans});
        }
        // Exact copy consumer exports its matching staging history.
    }
}
inline nlohmann::json VertexUploadHistory(uint64_t address,uint64_t bytes){
    std::scoped_lock lock(vertex_upload_mutex);
    auto result=nlohmann::json::array();
    for(auto it=vertex_upload_samples.rbegin();it!=vertex_upload_samples.rend()&&result.size()<16;++it)
        if(DemonsPrelightPolicy::Overlaps(address,bytes,it->address,it->bytes))
            result.push_back({{"address",it->address},{"bytes",it->bytes},{"tick",it->tick},
                {"word_digest",it->digest},{"nan_words",it->nans}});
    return result;
}
inline int VertexNanClass(size_t n,size_t total){
    if(!total)return -1;
    return n==0?0:(n==total?2:1);
}
inline void RecordVertexConsumer(nlohmann::json row){
    static std::mutex mutex;
    static std::vector<std::pair<uint64_t,int>> seen;
    std::scoped_lock lock(mutex);
    bool edge=false;
    for(auto& e:row["sources"]){
        if(e.value("slot",0u)!=2u||!e.contains("gpu_words"))continue;
        const auto& w=e["gpu_words"];const auto expected=e.value("bytes",0ull)/4;
        const auto klass=VertexNanClass(e.value("nan_words",0ull),w.size());
        if(w.size()!=expected||!expected)continue;
        const uint64_t address=e.value("address",0ull);
        auto it=std::ranges::find_if(seen,[&](const auto& v){return v.first==address;});
        if(it==seen.end()){seen.emplace_back(address,klass);edge=true;}
        else if(it->second!=klass){it->second=klass;edge=true;}
    }
    row["nan_class_edge"]=edge;
    if(!edge)for(auto& e:row["sources"])e.erase("gpu_words");
    VertexRecord(std::move(row));
}
inline void StatusLocked(const Journal& j) {
    std::ofstream out(Folder()/"ds_prelight_status.json",std::ios::trunc);
    if(out)out<<nlohmann::json{{"schema",3},{"node","N32/L09 vertex producer"},{"last_seq",j.seq},
        {"dropped_events",j.dropped},{"samples",j.samples},{"sample_dropped",j.sample_dropped},
        {"full_samples",j.full},{"full_dropped",j.full_dropped},{"chunks",j.chunk+(j.out.is_open()?1u:0u)},
        {"scope","observed target range; candidate cache events, not complete guest oracle"},
        {"root_decision","production_branch_observed_after_zero_clear_expected_geometry_still_unproven"}};
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
    if(!Enabled())return;
    if(VertexMatches(address,bytes))VertexRecord({{"kind","range"},{"operation",operation},{"tick",tick},{"address",address},{"bytes",bytes},{"detail",detail}});
    if(!Matches(address,bytes))return;
    Record({{"kind","range"},{"operation",operation},{"tick",tick},
        {"address",address},{"bytes",bytes},{"detail",detail}});
}
inline void BufferWriter(uint64_t tick,const char* op,uint64_t address,uint64_t bytes,
        uint64_t operand,const char* route,bool overflow=false){
    if(!Enabled())return;
    if(VertexMatches(address,bytes))VertexRecord({{"kind","buffer_writer"},{"operation",op},{"tick",tick},{"address",address},{"bytes",bytes},{"operand",operand},{"route",route},{"overflow",overflow}});
    if(!Matches(address,bytes))return;
    Record({{"kind","buffer_writer"},{"operation",op},{"tick",tick},
        {"address",address},{"bytes",bytes},{"operand",operand},{"route",route},{"overflow",overflow}});
}
// Distinguish guest draw admission from pipeline admission. Fallback is not a drop.
struct CommandScope {
    uint64_t old=command_seq;
    bool old_prepared=command_prepared;
    const char* outcome="returned_without_prepared";
    explicit CommandScope(nlohmann::json data){
        command_seq=data.is_null()?0:Record(std::move(data));
        command_prepared=false;
    }
    ~CommandScope(){
        if(command_seq)Record({{"kind","command_return"},{"command",command_seq},
            {"prepared",command_prepared},{"outcome",command_prepared?"prepared":outcome}});
        command_seq=old;command_prepared=old_prepared;
    }
};
void ObserveCompute(RenderContext& context,const PreparedBindings& bindings,uint64_t submit,
                    bool after,bool indirect,uint64_t args_address=0);
}
}
