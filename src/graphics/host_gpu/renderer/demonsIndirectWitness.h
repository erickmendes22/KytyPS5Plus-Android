#pragma once
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include "graphics/host_gpu/renderer/demonsIndirectWitnessPolicy.h"
#include "common/emulatorConfig.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::DemonsIndirectWitness {
using namespace DemonsIndirectWitnessPolicy;
using Json=nlohmann::json;
struct Target { uint64_t address=0,bytes=0,stream_id=0; };
inline bool Enabled(){
    return DemonsCausalMatrix::Mode()==DemonsCausalMatrix::Trace &&
        Config::GetShaderDumpFolder().filename()=="ds-lighting-dumps";
}
inline std::filesystem::path Folder(){return Config::GetShaderDumpFolder()/"queue-state";}
struct AtomicTarget {
    std::atomic<uint64_t> address{0},bytes{0},epoch{0};
    std::atomic<bool> sealed{false};
};
inline std::array<AtomicTarget,4> targets;
inline std::atomic<bool> selected_producer_pins{false};
inline std::atomic<uint64_t> order{0};
inline std::atomic<uint64_t> signal_dropped{0};
inline std::vector<Target> TargetRanges(){
    std::vector<Target> result;
    if(!Enabled())return result;
    for(size_t i=0;i<targets.size();++i){
        const auto a=targets[i].address.load(std::memory_order_acquire);
        if(a&&!targets[i].sealed.load(std::memory_order_acquire))result.push_back({a,targets[i].bytes.load(std::memory_order_relaxed),i+1});
    }
    return result;
}
inline bool Matches(uint64_t a,uint64_t n){
    if(!Enabled())return false;
    for(auto& t:targets)if(!t.sealed.load(std::memory_order_acquire)&&Overlap(a,n,t.address.load(std::memory_order_acquire),t.bytes.load(std::memory_order_relaxed)))return true;
    return false;
}
inline uint64_t Epoch(uint64_t stream){return stream&&stream<=targets.size()?targets[stream-1].epoch.load(std::memory_order_acquire):0;}
struct Blob { uint64_t bytes=0,digest=0,stream=0; bool current=false; };
struct Journal {
    std::mutex mutex;
    std::deque<std::string> events;
    uint64_t event_bytes=0,evicted=0,dropped=0,blob_dropped=0,io_errors=0;
    std::map<std::string,Blob> files;
    std::map<uint64_t,Json> producers;
    std::map<uint64_t,Json> producer_chains;
    std::map<uint64_t,uint64_t> chain_omitted;
    std::map<uint64_t,Retention> retention;
    std::map<uint64_t,Json> last_seen;
    std::map<uint64_t,uint32_t> inflight;
    Json observed_loss;
    std::map<std::string,Json> batches;
    std::vector<std::string> external;
    std::array<uint64_t,SelectedProducerPinLimit> recent{};
    Json retired_pins;
};
inline Journal& State(){static Journal state;return state;}
inline bool NameOk(const std::string& name){
    return name.size()<256 && name.starts_with("ds_prelight_cp_args_") && name.find('/')==std::string::npos &&
        name.find('\\')==std::string::npos && name.find("..") == std::string::npos;
}
inline uint64_t BlobBytesLocked(const Journal& state){uint64_t n=0;for(const auto& [name,blob]:state.files){(void)name;n+=blob.bytes;}return n;}
inline uint64_t BlobBytesRemaining(){auto& j=State();std::scoped_lock lock(j.mutex);return BlobBudget-std::min(BlobBudget,BlobBytesLocked(j));}
inline uint64_t CurrentBlobBytesRemaining(uint64_t stream){
    auto& j=State();std::scoped_lock lock(j.mutex);uint64_t n=0;
    for(const auto& [name,blob]:j.files){(void)name;if(blob.current&&blob.stream==stream)n+=blob.bytes;}
    return std::min(GenerationBudget-std::min(GenerationBudget,n),BlobBudget-std::min(BlobBudget,BlobBytesLocked(j)));
}
inline uint64_t ProducerBlobCapacity(uint64_t stream){
    auto& j=State();std::scoped_lock lock(j.mutex);uint64_t current=0;
    for(const auto& [name,blob]:j.files){(void)name;if(blob.current&&blob.stream==stream)current+=blob.bytes;}
    return ReplacementCapacity(BlobBytesLocked(j),current);
}
inline void RemoveLocked(Journal& j,const std::string& name){
    std::error_code ec;std::filesystem::remove(Folder()/name,ec);
    if(ec)++j.io_errors;
    j.files.erase(name);
}
inline void BeginProducer(uint64_t stream){
    if(!Enabled())return;
    auto& j=State();std::scoped_lock lock(j.mutex);
    std::vector<std::string> names;
    for(const auto& [name,blob]:j.files)if(blob.current&&blob.stream==stream)names.push_back(name);
    for(const auto& name:names)RemoveLocked(j,name);
    j.producers.erase(stream);
    j.producer_chains.erase(stream);
    j.chain_omitted.erase(stream);
}
inline size_t ProducerChainSize(uint64_t stream){
    auto& j=State();std::scoped_lock lock(j.mutex);
    const auto it=j.producer_chains.find(stream);
    return it==j.producer_chains.end()?0:it->second.size();
}
inline bool SaveBlob(const std::string& name,std::span<const uint8_t> data){
    if(!Enabled()||!NameOk(name))return false;
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(!j.files.contains(name)&&j.files.size()>=128){++j.blob_dropped;return false;}
    const auto old=j.files.contains(name)?j.files[name].bytes:0;
    uint64_t stream=0;
    constexpr std::string_view prefix="ds_prelight_cp_args_current_";
    if(name.starts_with(prefix)){
        const auto rest=name.substr(prefix.size());
        for(char c:rest){if(c<'0'||c>'9')break;stream=stream*10+uint64_t(c-'0');if(stream>4)break;}
    }
    uint64_t generation=0;
    for(const auto& [file,blob]:j.files)if(blob.current&&blob.stream==stream){(void)file;generation+=blob.bytes;}
    if(data.size()>GenerationBudget||BlobBytesLocked(j)-old+data.size()>BlobBudget||
        (stream&&generation-old+data.size()>GenerationBudget)){++j.blob_dropped;return false;}
    std::error_code ec;std::filesystem::create_directories(Folder(),ec);
    std::ofstream out(Folder()/name,std::ios::binary|std::ios::trunc);
    if(out&&!data.empty())out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
    out.close();
    if(ec||!out){++j.io_errors;return false;}
    j.files[name]={data.size(),Digest(data),stream,stream!=0};return true;
}
inline void RegisterRequired(const std::string& relative){
    if(!Enabled()||relative.empty()||relative.find("..")!=std::string::npos)return;
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(std::ranges::find(j.external,relative)==j.external.end()){
        if(j.external.size()<1024&&relative.size()<512)j.external.push_back(relative);else ++j.dropped;
    }
}
inline uint64_t Record(Json event){
    if(!Enabled())return 0;
    const auto seq=order.fetch_add(1,std::memory_order_acq_rel)+1;
    event["seq"]=seq;
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(event.value("kind",std::string{})=="producer"&&event.value("phase",std::string{})=="after"){
        const auto stream=event.value("stream_id",uint64_t{0});
        if(stream){
            if(event.value("retained",false)&&!j.producers.contains(stream))j.producers[stream]=event;
            auto& chain=j.producer_chains[stream];
            if(chain.is_null())chain=Json::array();
            if(chain.size()<ProducerChainLimit)chain.push_back(event);
            else ++j.chain_omitted[stream];
        }
    }
    auto line=event.dump();
    if(line.size()>512u*1024u){++j.dropped;return seq;}
    while(!j.events.empty()&&j.event_bytes+line.size()+1>4u*1024u*1024u){j.event_bytes-=j.events.front().size()+1;j.events.pop_front();++j.evicted;}
    j.event_bytes+=line.size()+1;j.events.push_back(std::move(line));return seq;
}
inline Target Watch(uint64_t address){
    if(!Enabled()||!ValidRange(address,BatchBytes))return {};
    auto& j=State();std::scoped_lock lock(j.mutex);
    RememberAddress(j.recent,address);
    if(!j.observed_loss.is_null()||selected_producer_pins.load())return {};
    for(size_t i=0;i<targets.size();++i)if(targets[i].address.load(std::memory_order_acquire)==address){
        if(targets[i].sealed.load(std::memory_order_acquire))return {};
        return {address,BatchBytes,i+1};
    }
    for(size_t i=0;i<targets.size();++i)if(!targets[i].address.load(std::memory_order_relaxed)){
        targets[i].bytes.store(BatchBytes,std::memory_order_relaxed);
        targets[i].address.store(address,std::memory_order_release);return {address,BatchBytes,i+1};
    }
    ++j.dropped;return {};
}
inline void BeginBatch(uint64_t stream){
    auto& j=State();std::scoped_lock lock(j.mutex);++j.inflight[stream];
}
inline void Mutation(uint64_t address,uint64_t bytes,const char* path){
    if(!Matches(address,bytes))return;
    Json affected=Json::array();
    for(size_t i=0;i<targets.size();++i)if(Overlap(address,bytes,targets[i].address.load(),targets[i].bytes.load())){
        const auto epoch=targets[i].epoch.fetch_add(1,std::memory_order_acq_rel)+1;
        affected.push_back({{"stream_id",i+1},{"epoch",epoch}});
    }
    Record({{"kind","mutation"},{"path",path},{"address",address},{"bytes",bytes},{"targets",affected},
        {"scope","observed writer claim; not a proof of an executed shader store"}});
}
// These hooks can run in a fault handler. No allocation, filesystem, renderer
// calls or locks. A slot is published only after its fixed payload is complete.
// Overflow is explicit; the witness does not claim to observe every CPU store.
enum class SignalKind:uint8_t {FaultRead,FaultWrite,ReadMemory,WriteBacking,PublicationQueued,PublicationDone,Upload};
struct SignalEvent {
    std::atomic<uint8_t> state{0};
    uint64_t seq=0,address=0,bytes=0,stream=0,epoch=0,detail=0;
    SignalKind kind{};bool ok=false;uint8_t payload_size=0;
    std::array<uint8_t,60> payload{};
};
inline std::array<SignalEvent,1024> signal_events;
inline std::atomic<uint64_t> signal_cursor{0};
inline void Signal(SignalKind kind,uint64_t address,uint64_t bytes,bool ok,uint64_t detail=0,const void* payload=nullptr) noexcept {
    for(size_t i=0;i<targets.size();++i){
        const auto a=targets[i].address.load(std::memory_order_acquire),n=targets[i].bytes.load(std::memory_order_relaxed);
        if(targets[i].sealed.load(std::memory_order_acquire)||!Overlap(address,bytes,a,n))continue;
        if(kind==SignalKind::FaultWrite||kind==SignalKind::WriteBacking||kind==SignalKind::Upload)targets[i].epoch.fetch_add(1,std::memory_order_acq_rel);
        auto& event=signal_events[signal_cursor.fetch_add(1,std::memory_order_relaxed)%signal_events.size()];
        uint8_t expected=0;
        if(!event.state.compare_exchange_strong(expected,1,std::memory_order_acquire)){signal_dropped.fetch_add(1,std::memory_order_relaxed);continue;}
        event.seq=order.fetch_add(1,std::memory_order_acq_rel)+1;
        event.kind=kind;event.address=address;event.bytes=bytes;event.ok=ok;event.detail=detail;event.stream=i+1;
        event.epoch=targets[i].epoch.load(std::memory_order_acquire);event.payload_size=0;
        if(payload){
            const auto begin=std::max(a,address),end=std::min(a+n,address+bytes);
            event.address=begin;event.bytes=end-begin;event.payload_size=static_cast<uint8_t>(end-begin);
            std::memcpy(event.payload.data(),static_cast<const uint8_t*>(payload)+(begin-address),event.payload_size);
        }
        event.state.store(2,std::memory_order_release);
    }
}
inline void DrainSignals(){
    static constexpr const char* names[]={"fault_read","fault_write_candidate","read_memory_result","write_backing","publication_queued","publication_done","backing_upload"};
    for(auto& event:signal_events){
        uint8_t expected=2;if(!event.state.compare_exchange_strong(expected,3,std::memory_order_acquire))continue;
        Json row={{"kind",names[static_cast<unsigned>(event.kind)]},{"observed_seq",event.seq},{"address",event.address},
            {"bytes",event.bytes},{"stream_id",event.stream},{"epoch",event.epoch},{"ok",event.ok},{"detail",event.detail}};
        if(event.payload_size)row["raw_bytes"]=std::vector<uint8_t>(event.payload.begin(),event.payload.begin()+event.payload_size);
        if(event.kind==SignalKind::FaultWrite)row["candidate_only"]=true;
        if(event.kind==SignalKind::ReadMemory){row["downloaded"]=(event.detail&1)!=0;row["is_write"]=(event.detail&2)!=0;}
        if(event.kind==SignalKind::PublicationQueued||event.kind==SignalKind::PublicationDone)row["publication_token"]=event.detail;
        Record(std::move(row));event.state.store(0,std::memory_order_release);
    }
}
struct Fallback {uint64_t address=0,command=0,submit=0,tick=0;std::string reason;};
inline thread_local Fallback last_fallback;
inline void NoteFallback(uint64_t address,const char* reason,uint64_t command,uint64_t submit,uint64_t tick){
    if(Enabled())last_fallback={address,command,submit,tick,reason?reason:"unspecified"};
}
inline Json UsesJson(const Uses& uses){
    constexpr const char* fields[]={"index_count","instance_count","first_index","base_vertex","first_instance"};
    constexpr const char* phases[]={"log","patch","index_address","count","instances","empty","draw"};
    auto result=Json::array();
    for(size_t i=0;i<uses.count;++i){const auto& use=uses.entries[i];result.push_back({{"field",fields[use.field]},{"phase",phases[use.phase]},{"value",use.value}});}
    return result;
}
inline void FlushLocked(Journal& j){
    std::error_code ec;std::filesystem::create_directories(Folder(),ec);
    const std::string events="ds_prelight_cp_args_events.jsonl";
    Json required=Json::array(),batches=Json::array(),retention=Json::array();
    uint64_t batch_bytes=0;
    std::vector<std::pair<std::string,std::string>> serialized;
    for(const auto& [name,blob]:j.files)required.push_back({{"file","queue-state/"+name},{"bytes",blob.bytes},{"fnv1a64",blob.digest}});
    for(const auto& [name,batch]:j.batches){
        const auto data=batch.dump();batch_bytes+=data.size();serialized.emplace_back(name,data);
        required.push_back({{"file","queue-state/"+name},{"bytes",data.size()},
            {"fnv1a64",Digest({reinterpret_cast<const uint8_t*>(data.data()),data.size()})}});
        batches.push_back({{"parent_command",batch.value("parent_command",uint64_t{0})},{"file","queue-state/"+name}});
    }
    for(const auto& file:j.external)required.push_back({{"file",file}});
    for(const auto& [stream,r]:j.retention)retention.push_back({{"stream_id",stream},{"last_nonzero",r.last_nonzero},
        {"first_zero",r.first_zero},{"sealed",r.sealed},{"last_seen",j.last_seen[stream]}});
    Json manifest;
    std::string manifest_text;
    for(;;){
        uint64_t events_digest=14695981039346656037ull;
        for(const auto& line:j.events){for(unsigned char c:line){events_digest^=c;events_digest*=1099511628211ull;}events_digest^='\n';events_digest*=1099511628211ull;}
        auto closure=required;
        closure.push_back({{"file","queue-state/"+events},{"bytes",j.event_bytes},{"fnv1a64",events_digest}});
        manifest={{"schema",1},{"scope","local CP 60-byte read edge; no full CPU store or root P8 certificate"},
            {"required_files",closure},{"batches",batches},{"retention",retention},{"observed_loss",j.observed_loss},{"prehistory_evicted",j.evicted},
            {"drops",{{"events",j.dropped},{"blobs",j.blob_dropped},{"signals",signal_dropped.load()},{"io_errors",j.io_errors}}},
            {"budgets",{{"metadata",MetadataBudget},{"blobs",BlobBudget},{"blob_used",BlobBytesLocked(j)},
                {"retained_batch_bytes",batch_bytes},{"event_bytes",j.event_bytes}}}};
        manifest_text=manifest.dump();
        // Actual serialized bytes, including the manifest and attached producer
        // metadata. Prefer the pinned endpoints over expendable rolling history.
        if(MetadataFits(batch_bytes,j.event_bytes,manifest_text.size()))break;
        if(j.events.empty()){
            // Batches are capped after attachments and there are at most eight;
            // this is a corruption/future-schema guard, not silent truncation.
            ++j.dropped;
            std::ofstream failure(Folder()/"ds_prelight_cp_args_manifest.json",std::ios::trunc);
            failure<<Json{{"schema",1},{"metadata_failure","retained_payload_exceeds_budget"},
                {"drops",{{"events",j.dropped}}},{"required_files",Json::array()},{"batches",Json::array()}}.dump();
            return;
        }
        j.event_bytes-=j.events.front().size()+1;j.events.pop_front();++j.evicted;
    }
    std::ofstream out(Folder()/events,std::ios::trunc);
    for(const auto& line:j.events)out<<line<<'\n';
    out.close();if(ec||!out)++j.io_errors;
    for(const auto& [name,data]:serialized){
        std::ofstream file(Folder()/name,std::ios::trunc);file<<data;file.close();if(!file)++j.io_errors;
    }
    manifest["drops"]["io_errors"]=j.io_errors;
    manifest["selected_producer_pins"]=selected_producer_pins.load();
    manifest["retired_pins"]=j.retired_pins;
    std::ofstream status(Folder()/"ds_prelight_cp_args_manifest.json",std::ios::trunc);status<<manifest.dump();
}
// Retire the exhausted launch-history pins once, at the selected window.
// Old captures remain separately reproducible from their original ZIPs.
// Reserve the full blob budget for up to two live addresses and their pinned
// endpoint copies (2 * 2 * 16 MiB). Never evict a live pin mid-window.
inline bool ArmSelectedProducers(){
    if(!Enabled())return false;
    if(selected_producer_pins.load(std::memory_order_acquire))return true;
    DrainSignals();
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(selected_producer_pins.load(std::memory_order_relaxed))return true;
    for(const auto& [stream,n]:j.inflight){(void)stream;if(n)return false;}
    j.retired_pins={{"blob_bytes",BlobBytesLocked(j)},{"batches",j.batches.size()},
        {"event_drops",j.dropped},{"blob_drops",j.blob_dropped}};
    for(auto& target:targets)target.sealed.store(true,std::memory_order_release);
    for(const auto& [name,batch]:j.batches){
        (void)batch;std::error_code ec;std::filesystem::remove(Folder()/name,ec);
        if(ec)++j.io_errors;
    }
    while(!j.files.empty()){const auto name=j.files.begin()->first;RemoveLocked(j,name);}
    j.batches.clear();j.producers.clear();j.producer_chains.clear();j.chain_omitted.clear();
    j.retention.clear();j.last_seen.clear();j.inflight.clear();j.observed_loss=nullptr;
    j.evicted+=j.events.size();j.events.clear();j.event_bytes=0;
    // Report historical drops separately; new counters cover the selected window.
    j.dropped=0;j.blob_dropped=0;
    for(size_t i=0;i<targets.size();++i){
        const auto address=i<j.recent.size()?j.recent[i]:0;
        targets[i].bytes.store(address?BatchBytes:0,std::memory_order_relaxed);
        targets[i].address.store(address,std::memory_order_release);
        targets[i].sealed.store(false,std::memory_order_release);
    }
    selected_producer_pins.store(true,std::memory_order_release);
    FlushLocked(j);return true;
}
inline Target SelectedProducerTarget(uint64_t address){
    if(!selected_producer_pins.load()||!ValidRange(address,BatchBytes))return {};
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(!j.observed_loss.is_null())return {};
    for(size_t i=0;i<SelectedProducerPinLimit;++i)
        if(targets[i].address.load()==address&&!targets[i].sealed.load())
            return {address,BatchBytes,i+1};
    for(size_t i=0;i<SelectedProducerPinLimit;++i)
        if(!targets[i].address.load()){
            targets[i].bytes.store(BatchBytes,std::memory_order_relaxed);
            targets[i].address.store(address,std::memory_order_release);
            return {address,BatchBytes,i+1};
        }
    return {};
}
inline void SaveBatch(Json batch){
    if(!Enabled())return;
    DrainSignals();
    batch["kind"]="cp_batch";batch["schema"]=1;
    if(batch.dump().size()>256u*1024u){auto& j=State();std::scoped_lock lock(j.mutex);++j.dropped;FlushLocked(j);return;}
    const auto stream=batch.value("stream_id",uint64_t{0});
    const auto seq=Record(batch);batch["seq"]=seq;
    bool nonzero=false;
    for(const auto& record:batch["records"])nonzero|=record["emitted"].value("index_count",0u)!=0 && record["emitted"].value("instance_count",0u)!=0;
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(j.inflight[stream])--j.inflight[stream];
    j.last_seen[stream]={{"seq",seq},{"frame",batch.value("frame",0u)},
        {"submit",batch.value("submit",uint64_t{0})},{"parent_command",batch.value("parent_command",uint64_t{0})}};
    auto& retention=j.retention[stream];
    if(retention.Observe(seq,nonzero)){
        if(nonzero){
            const auto obsolete="ds_prelight_cp_args_first_zero_"+std::to_string(stream)+"_";
            std::vector<std::string> erase;
            for(const auto& [name,blob]:j.files){(void)blob;if(name.starts_with(obsolete))erase.push_back(name);}
            for(const auto& name:erase)RemoveLocked(j,name);
            const auto batch_name=obsolete+"batch.json";
            j.batches.erase(batch_name);
            std::error_code ec;std::filesystem::remove(Folder()/batch_name,ec);if(ec)++j.io_errors;
        }
        const std::string slot=nonzero?"last_nonzero":"first_zero";
        const auto prefix="ds_prelight_cp_args_"+slot+"_"+std::to_string(stream)+"_";
        std::vector<std::string> old;
        for(const auto& [name,blob]:j.files){(void)blob;if(name.starts_with(prefix))old.push_back(name);}
        for(const auto& name:old)RemoveLocked(j,name);
        Json pinned=Json::array();
        std::vector<std::pair<std::string,Blob>> current;
        for(const auto& [name,blob]:j.files)if(blob.current&&blob.stream==stream)current.emplace_back(name,blob);
        for(const auto& [name,blob]:current){
            const auto destination=prefix+name.substr(std::string("ds_prelight_cp_args_current_"+std::to_string(stream)+"_").size());
            if(BlobBytesLocked(j)+blob.bytes>BlobBudget){++j.blob_dropped;continue;}
            std::error_code ec;std::filesystem::copy_file(Folder()/name,Folder()/destination,std::filesystem::copy_options::overwrite_existing,ec);
            if(ec){++j.io_errors;continue;}
            j.files[destination]={blob.bytes,blob.digest,stream,false};
            pinned.push_back({{"file","queue-state/"+destination},{"source_name","queue-state/"+name},{"bytes",blob.bytes},{"fnv1a64",blob.digest}});
        }
        batch["pinned_producer_files"]=pinned;
        if(j.producers.contains(stream))batch["last_observed_producer"]=j.producers[stream];
        if(j.producer_chains.contains(stream))batch["producer_chain"]=j.producer_chains[stream];
        batch["producer_chain_omitted"]=j.chain_omitted[stream];
        batch["retention_slot"]=slot;
        if(batch.dump().size()>512u*1024u){
            batch.erase("last_observed_producer");
            batch.erase("producer_chain");
            batch["producer_capture_failure"]="attached_producer_metadata_exceeds_retained_limit";
            ++j.blob_dropped;
        }
        j.batches[prefix+"batch.json"]=std::move(batch);
    }
    FlushLocked(j);
    if(!j.observed_loss.is_null()){
        retention.Seal();if(stream&&stream<=targets.size())targets[stream-1].sealed.store(true,std::memory_order_release);
        FlushLocked(j);
    }
}
inline void SealAtLoss(int32_t frame,uint32_t queue){
    if(!Enabled())return;
    DrainSignals();
    const auto seq=Record({{"kind","observed_loss"},{"frame",frame},{"queue",queue}});
    auto& j=State();std::scoped_lock lock(j.mutex);
    if(!j.observed_loss.is_null())return;
    j.observed_loss={{"frame",frame},{"queue",queue},{"seq",seq}};
    for(auto& [stream,retention]:j.retention){
        if(!j.inflight[stream]){
            retention.Seal();if(stream&&stream<=targets.size())targets[stream-1].sealed.store(true,std::memory_order_release);
        }
    }
    FlushLocked(j);
}
}
