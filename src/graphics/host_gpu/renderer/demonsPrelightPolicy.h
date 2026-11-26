#pragma once
#include <cstdint>
#include <span>
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
