#pragma once
#include "graphics/host_gpu/renderer/demonsRayCopyPolicy.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::DemonsRaySourceHistory {
using namespace DemonsRayCopyPolicy;
constexpr uint32_t RowWords=BytesPerProbe/4;
struct Summary {
    uint64_t digest=14695981039346656037ull;
    std::array<uint32_t,2> colored{},high_nibble{},finite_depth{};
};
constexpr Summary Summarize(std::span<const uint32_t> words) {
    Summary s;
    if(words.size()!=RowWords)return s;
    for(uint32_t i=0;i<RaysPerProbe;++i){
        const auto lo=words[2*i],hi=words[2*i+1];
        s.digest=(s.digest^((uint64_t{hi}<<32)|lo))*1099511628211ull;
        s.colored[i/128]+=(lo&0xffffffu)!=0;
        s.high_nibble[i/128]+=(lo>>28)!=0;
        const auto d=(hi>>16)&0x7fffu;
        s.finite_depth[i/128]+=d!=0&&d<0x7c00;
    }
    return s;
}
constexpr uint32_t DifferentPairs(std::span<const uint32_t> a,std::span<const uint32_t> b) {
    if(a.size()!=RowWords||b.size()!=RowWords)return UINT32_MAX;
    uint32_t n=0;
    for(size_t i=0;i<RowWords;i+=2)n+=a[i]!=b[i]||a[i+1]!=b[i+1];
    return n;
}
struct Record {
    int32_t frame=-1;
    uint64_t submit=0,tick=0,source_guest=0;
    uint32_t source_row=NoRow,map_word=0,dispatch=0;
    Summary input{},output{},backing{};
    uint32_t input_output_diff=UINT32_MAX,input_backing_diff=UINT32_MAX;
    bool valid=false;
};
class History {
public:
    void Invalidate(){owner=0;rows.clear();}
    void Begin(uint64_t address){
        if(owner!=address||rows.size()!=ProbeCount){
            owner=address;rows.assign(ProbeCount,Record{});
        }
    }
    void Put(uint32_t probe,Record r){
        if(probe>=rows.size())return;
        auto& old=rows[probe];
        if(old.frame==r.frame&&old.dispatch==r.dispatch)r.valid=false;
        old=r;
    }
    const Record* Find(uint64_t address,uint32_t probe)const{
        if(owner!=address||probe>=rows.size()||!rows[probe].valid)return nullptr;
        return &rows[probe];
    }
private:
    uint64_t owner=0;
    std::vector<Record> rows;
};
}
