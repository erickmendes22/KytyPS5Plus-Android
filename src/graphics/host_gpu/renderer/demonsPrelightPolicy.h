#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace Libs::Graphics::DemonsPrelightPolicy {
// A word is evidence only of stored bytes, never proof of expected albedo.
// Preserve alpha separately: RGB=0 can be intentional.
constexpr bool Colored(uint32_t w) {return (w&0xffffffu)!=0;}
struct Delta {bool valid=false;uint64_t changed=0,lost=0,gained=0,before=0,after=0;};
inline Delta Compare(std::span<const uint32_t> a,std::span<const uint32_t> b) {
    Delta d;
    if(a.empty()||a.size()!=b.size())return d;
    d.valid=true;
    for(size_t i=0;i<a.size();++i){
        const bool ca=Colored(a[i]),cb=Colored(b[i]);
        d.before+=ca;d.after+=cb;d.changed+=a[i]!=b[i];d.lost+=ca&&!cb;d.gained+=!ca&&cb;
    }
    return d;
}
// Null descriptors are legal placeholders in prepared bindings. Diagnostic
// code must not pass empty or wrapping ranges into RangeSet::End.
constexpr bool DiagnosticRange(uint64_t address,uint64_t bytes){
    return address!=0&&bytes!=0&&bytes<=UINT64_MAX-address;
}
constexpr bool DiagnosticSlice(uint64_t offset,uint64_t range,uint64_t adjust,uint64_t bytes){
    return bytes!=0&&range!=UINT64_MAX&&adjust<=range&&bytes<=range-adjust&&
        adjust<=UINT64_MAX-offset&&bytes<=UINT64_MAX-(offset+adjust)&&
        ((offset+adjust)&3u)==0&&(bytes&3u)==0;
}
constexpr bool Overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m){
    return n&&m&&(a<=b?b-a<n:a-b<m);
}
// Capture windows follow measured layer-0 loss, never fixed host frame numbers.
// This is a scheduling criterion, NOT a claim about the expected scene.
class ProductionWindow {
public:
    constexpr void Observe(int32_t frame,uint64_t colored,uint64_t area){
        if(frame<0||!area||colored>area||frame==last_frame)return;
        if(frame<last_frame)*this={};
        last_frame=frame;
        if(colored>=area/2){bright=true;return;}
        if(bright&&colored<=area/10&&count<2){
            ++count;start=frame;bright=false;
        }
    }
    constexpr int Phase(int32_t frame)const {
        return count&&frame>=start&&int64_t{frame}-start<=5?count:-1;
    }
private:
    bool bright=false;
    int32_t last_frame=-1,start=-1,count=0;
};
// V3: the first observed drop is a retention trigger, never a FAIL oracle.
class BisectionWindow {
public:
    constexpr bool Observe(int32_t frame,uint32_t queue,uint64_t colored,uint64_t area){
        if(frame<0||!area||colored>area||(queue!=0&&queue!=33))return false;
        if(edge>=0)return false;
        const auto q=queue==0?0u:1u;
        if(colored>=area/2){seen_bright[q]=true;armed=true;return false;}
        if(seen_bright[q]&&colored<=area/10){edge=frame;edge_queue=queue;return true;}
        return false;
    }
    constexpr int Phase(int32_t frame) const {
        if(!armed)return -1;
        if(edge<0)return 0;
        return frame<=edge+2?1:-1;
    }
    constexpr bool Closed(int32_t frame)const{return edge>=0&&frame>edge+4;}
    constexpr int32_t Edge()const{return edge;}
    constexpr uint32_t Queue()const{return edge_queue;}
private:
    bool seen_bright[2]{};
    bool armed=false;
    int32_t edge=-1;
    uint32_t edge_queue=UINT32_MAX;
};
// A draw's inputs are captured before render-target acquisition. Only the executed
// clear is authoritative for the cycle boundary (#741/#744: false -> true).
struct ExecutedCycle {
    uint64_t first=0;
    constexpr bool Observe(uint64_t draw,bool depth_target,bool executed_clear){
        if(!draw||!depth_target||!executed_clear)return false;
        first=draw;return true;
    }
    constexpr bool Includes(uint64_t draw,uint64_t loss) const {
        return first&&draw>=first&&draw<=loss;
    }
};
// A prepared input stays outside the previous cycle's budget until the
// executed clear is known. Otherwise a full previous cycle rejects exactly
// the first record needed to establish the next cycle (#741/#744 boundary).
struct CycleInputRetention {
    struct Limits {
        uint64_t bytes=48u*1024u*1024u;
        uint64_t record_bytes=4u*1024u*1024u;
        std::size_t records=192;
    };
    uint64_t bytes=0,dropped=0;
    std::vector<std::pair<uint64_t,std::string>> inputs;

    bool Retain(uint64_t draw,std::string payload,bool begins_cycle,
                bool cycle_selected,Limits limits) {
        if(begins_cycle){inputs.clear();bytes=0;dropped=0;}
        const uint64_t size=payload.size();
        if(!draw||payload.empty()||size>limits.record_bytes||size>limits.bytes||!limits.records){
            ++dropped;return false;
        }
        // Before a confirmed boundary only a rolling prefix is meaningful.
        // Once selected, missing records must be reported, never evicted.
        if(!cycle_selected){
            while(!inputs.empty()&&(bytes>limits.bytes||size>limits.bytes-bytes||inputs.size()>=limits.records)){
                bytes-=inputs.front().second.size();inputs.erase(inputs.begin());
            }
        }
        if(bytes>limits.bytes||size>limits.bytes-bytes||inputs.size()>=limits.records){++dropped;return false;}
        bytes+=size;inputs.emplace_back(draw,std::move(payload));return true;
    }
};
enum class Certificate {Blocked,Pass,Fail};
struct Interval {
    unsigned lo=0,hi=16;
    constexpr unsigned Mid()const{return lo+(hi-lo)/2;}
    constexpr bool Apply(Certificate c){
        if(hi-lo<=1||c==Certificate::Blocked)return false;
        const auto mid=Mid();
        if(c==Certificate::Pass)lo=mid;else hi=mid;
        return true;
    }
};
// No binary branch may be selected merely from a zero observation.
// A verified expected word and a complete same-cycle history are required.
enum class Root {Blocked,Produced,NotProduced};
constexpr Root Decide(bool expected_valid,bool history_complete,bool same_identity,
                      bool observed_expected,bool consumer_matches_expected){
    if(!expected_valid||!history_complete||!same_identity||consumer_matches_expected)
        return Root::Blocked;
    return observed_expected?Root::Produced:Root::NotProduced;
}
}
