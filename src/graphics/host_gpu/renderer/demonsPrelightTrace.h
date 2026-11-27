#pragma once
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include "graphics/host_gpu/renderer/demonsP8Capture.h"
#include "graphics/host_gpu/renderer/demonsQueueProbe.h"
#include "graphics/host_gpu/renderer/demonsPrelightPolicy.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "common/emulatorConfig.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <algorithm>
#include <vector>
#include <deque>

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
    size_t bytes=0,rolling_bytes=0;
    uint64_t evicted=0,first_retained=0,closed_events=0;
    int32_t edge=-1,last_consumer=-1;
    uint32_t edge_queue=UINT32_MAX;
    bool triggered=false,closed=false;
    unsigned chunk=0;
    std::deque<std::pair<uint64_t,std::string>> rolling;
    std::ofstream out;
};
inline Journal& State(){static Journal j;return j;}
inline constexpr size_t ChunkBytes=2u*1024u*1024u;
inline constexpr unsigned MaxChunks=12;
inline constexpr size_t RollingBytes=12u*1024u*1024u;
inline thread_local uint64_t active_draw=0;
inline thread_local uint64_t command_seq=0;
inline thread_local bool command_prepared=false;
inline std::mutex phase_mutex;
inline DemonsPrelightPolicy::BisectionWindow production_window;
inline int Phase(int32_t frame){
    if(DemonsP8Capture::available.load()){
        DemonsP8Capture::BeginEvidence(frame);
        if(!DemonsP8Capture::ProducerFrame(frame))return -1;
        return frame==DemonsP8Capture::target.load()?1:0;
    }
    std::scoped_lock lock(phase_mutex);return production_window.Phase(frame);
}
// Compact command identity must outlive the expensive producer phase.
inline int CommandPhase(int32_t frame){
    if(DemonsP8Capture::available.load()){
        if(!DemonsP8Capture::BeginEvidence(frame))return -1;
        return frame==DemonsP8Capture::target.load()?1:0;
    }
    return Phase(frame);
}
inline bool ObservePopulation(int32_t frame,uint32_t queue,uint64_t colored,uint64_t area);
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
// P8 evidence must not expire when the older layer-0 witness is sealed.
// These are candidate windows, not an independent correctness oracle.
inline bool P8Frame(int32_t frame) {
    return DemonsP8Capture::FullFrame(frame) || DemonsP8Capture::FlipFrame(frame) ||
        (frame >= 0 && frame % 24 == 1);
}
inline void RecordP8Boundary(int32_t frame, nlohmann::json event) {
    if (!Enabled() || !P8Frame(frame)) return;
    struct Slot { int32_t frame=-1; size_t bytes=0; uint64_t rows=0, dropped=0; };
    static std::mutex mutex;
    static std::array<Slot,16> slots{};
    std::scoped_lock lock(mutex);
    const auto index=static_cast<size_t>(frame/24)%slots.size();
    auto& slot=slots[index];
    if (slot.frame>frame) return; // a late queue must not erase a newer frame
    const bool fresh=slot.frame!=frame;
    if(fresh) slot={frame,0,0,0};
    event["frame"]=frame;
    event["window_row"]=slot.rows;
    const auto line=event.dump();
    constexpr size_t budget=1024u*1024u;
    std::error_code ec;
    std::filesystem::create_directories(Folder(),ec);
    const auto stem="ds_prelight_p8_window_"+std::to_string(index);
    if(ec || line.size()+1>budget-slot.bytes) {
        ++slot.dropped;
        if(fresh && !ec) {
            // A fresh rejected row must not leave the prior frame's file in place.
            std::ofstream clear(Folder()/(stem+".jsonl"),std::ios::trunc);
        }
    } else {
        std::ofstream out(Folder()/(stem+".jsonl"),fresh?std::ios::trunc:std::ios::app);
        if(out) { out<<line<<'\n';out.flush(); }
        if(out) {slot.bytes+=line.size()+1;++slot.rows;} else ++slot.dropped;
    }
    std::ofstream status(Folder()/(stem+"_status.json"),std::ios::trunc);
    if(status)status<<nlohmann::json{{"frame",slot.frame},{"rows",slot.rows},{"bytes",slot.bytes},
        {"dropped",slot.dropped},{"scope","P8 boundary observations, not reference values"}};
}
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
    if(out)out<<nlohmann::json{{"schema",4},{"node","V3/G0/P8"},{"last_seq",j.seq},
        {"triggered",j.triggered},{"closed",j.closed},{"edge_frame",j.edge},{"edge_queue",j.edge_queue},
        {"last_consumer_frame",j.last_consumer},{"first_retained_seq",j.first_retained},
        {"prehistory_evicted",j.evicted},{"postwindow_ignored",j.closed_events},
        {"dropped_events",j.dropped},{"samples",j.samples},{"sample_dropped",j.sample_dropped},
        {"full_samples",j.full},{"full_dropped",j.full_dropped},{"chunks",j.chunk+(j.out.is_open()?1u:0u)},
        {"scope","bounded producer history and exact consumer snapshots; no automatic causal certificate"},
        {"selected_boundary",DemonsP8Capture::Boundary()},
        {"root_decision","BLOCKED_UNTIL_SAME_WITNESS_EXPECTED_OUTPUT_CERTIFIED"}};
}
inline bool WriteLocked(Journal& j,const std::string& line) {
    if(line.size()+1>ChunkBytes){++j.dropped;return false;}
    if(!j.out.is_open()||j.bytes+line.size()+1>ChunkBytes){
        if(j.out.is_open()){j.out.close();++j.chunk;}
        if(j.chunk>=MaxChunks){++j.dropped;return false;}
        std::error_code ec;std::filesystem::create_directories(Folder(),ec);
        j.out.open(Folder()/("ds_prelight_events_"+std::to_string(j.chunk)+".jsonl"),std::ios::trunc);
        j.bytes=0;
    }
    if(!j.out){++j.dropped;return false;}
    j.out<<line<<'\n';j.out.flush();
    if(!j.out){++j.dropped;return false;}
    j.bytes+=line.size()+1;return true;
}
inline bool ObservePopulation(int32_t frame,uint32_t queue,uint64_t colored,uint64_t area){
    // The selected window is already being written. Do not re-latch an
    // unrelated temporal layer-zero drop from an earlier scene.
    if(DemonsP8Capture::available.load()){
        auto& j=State();std::scoped_lock lock(j.mutex);j.last_consumer=frame;
        const bool selected=DemonsP8Capture::boundary_reached.load()&&!j.triggered;
        if(selected){j.triggered=true;j.edge=frame;j.edge_queue=queue;}
        StatusLocked(j);return selected;
    }
    bool edge=false,close=false;
    {std::scoped_lock lock(phase_mutex);edge=production_window.Observe(frame,queue,colored,area);
        close=production_window.Closed(frame);}
    auto& j=State();std::scoped_lock lock(j.mutex);j.last_consumer=frame;
    if(edge&&!j.triggered){
        j.triggered=true;j.edge=frame;j.edge_queue=queue;
        j.first_retained=j.rolling.empty()?j.seq+1:j.rolling.front().first;
        for(const auto& entry:j.rolling)WriteLocked(j,entry.second);
        j.rolling.clear();j.rolling_bytes=0;
    }
    if(close)j.closed=true;
    if(!j.triggered){
        // The RAM ring used to disappear if the process exited before the
        // trigger. Keep a compact latest checkpoint without flushing 12 MiB.
        std::error_code ec;std::filesystem::create_directories(Folder(),ec);
        std::ofstream recent(Folder()/"ds_prelight_recent.jsonl",std::ios::trunc);
        if(recent){
            size_t n=0,bytes=0;
            for(auto i=j.rolling.rbegin();i!=j.rolling.rend()&&n<128;++i){
                if(bytes+i->second.size()+1>512u*1024u)break;
                ++n;bytes+=i->second.size()+1;
            }
            for(size_t i=j.rolling.size()-n;i<j.rolling.size();++i)recent<<j.rolling[i].second<<'\n';
        }
    }
    StatusLocked(j);return edge;
}
inline uint64_t Record(nlohmann::json event) {
    if(!Enabled())return 0;
    const bool selected=DemonsP8Capture::available.load();
    if(selected&&!DemonsP8Capture::evidence_started.load())return 0;
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(selected&&DemonsP8Capture::stopped.load()){
        if(!j.closed){j.closed=true;StatusLocked(j);}
        return 0;
    }
    if(j.closed){++j.closed_events;return 0;}
    if(selected&&event.contains("frame")&&event["frame"].is_number_integer()&&
        !DemonsP8Capture::EvidenceFrame(event["frame"].get<int32_t>()))return 0;
    const auto seq=++j.seq;event["seq"]=seq;
    if(selected&&seq==1)j.first_retained=seq;
    auto line=event.dump();
    if(!j.triggered&&!selected){
        if(line.size()+1>ChunkBytes){++j.dropped;return 0;}
        while(!j.rolling.empty()&&j.rolling_bytes+line.size()+1>RollingBytes){
            j.rolling_bytes-=j.rolling.front().second.size()+1;j.rolling.pop_front();++j.evicted;
        }
        j.rolling_bytes+=line.size()+1;j.rolling.emplace_back(seq,std::move(line));
    }else if(!WriteLocked(j,line)){StatusLocked(j);return 0;}
    if(seq==1||seq%128==0||event.value("kind",std::string{})=="consumer"){
        std::error_code ec;std::filesystem::create_directories(Folder(),ec);StatusLocked(j);
    }
    return seq;
}
inline bool AllowSample(bool full=false){
    if(!Enabled())return false;
    if(DemonsP8Capture::available.load()&&(!DemonsP8Capture::evidence_started.load()||
        DemonsP8Capture::stopped.load()))return false;
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(j.closed)return false;
    auto& used=full?j.full:j.samples;
    // No launch-global quota: the rolling window retains the actual onset.
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
