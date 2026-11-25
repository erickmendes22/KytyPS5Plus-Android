#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <bit>
namespace Libs::Graphics::DemonsTransitionTrace {
constexpr uint32_t RowWords=16;
constexpr bool ValidRow(std::span<const uint32_t> row,uint32_t probe) {
    return row.size()==RowWords&&row[14]==probe;
}
constexpr uint32_t GroupSummary(std::span<const uint32_t> row) {
    uint32_t bits=0;
    if(row.size()!=RowWords)return bits;
    for(uint32_t i=0;i<13;++i)if(row[i])bits|=1u<<i;
    return bits;
}
struct Counts {uint32_t rows=0,valid=0,group1=0,group1_lights=0,inconsistent=0;};
constexpr Counts Summarize(std::span<const uint32_t> words,std::span<const uint32_t> probes){
    Counts c;
    for(uint32_t i=0;i<probes.size()&&i<words.size()/RowWords;++i){
        const auto row=words.subspan(i*RowWords,RowWords);++c.rows;
        if(!ValidRow(row,probes[i]))continue;
        ++c.valid;c.group1+=bool(row[13]&2u);c.group1_lights+=std::popcount(row[1]);
        // Only compare the summary's group-1 bit with the actual group's word.
        c.inconsistent+=bool(row[13]&2u)!=bool(row[1]);
    }
    return c;
}
constexpr bool ExactWriter(std::span<const uint32_t> consumer,std::span<const uint32_t> producer){
    if(consumer.empty()||consumer.size()>producer.size())return false;
    for(uint32_t i=0;i<consumer.size();++i)if(consumer[i]!=producer[i])return false;
    return true;
}
constexpr uint64_t Pointer(uint32_t lo,uint32_t hi){return uint64_t{lo}|(uint64_t{hi}<<32);}
constexpr uint64_t Descriptor224Address(uint64_t root){
    return root&&root<=UINT64_MAX-0xc40?root+0xc30:0;
}
}
