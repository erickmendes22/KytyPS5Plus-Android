#pragma once
#include "kernel/addressFold.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::LibKernel::DemonsFileReadHistory {
struct Event {
    uint64_t id=0,begin=0,end=0,file_offset=0;
    std::string path;
};
inline bool Enabled(){
    static const bool enabled=[] {const char* v=std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");
        return v&&v[0]=='1'&&v[1]=='\0';}();
    return enabled;
}
inline std::mutex mutex;
inline std::array<Event,4096> events;
inline uint64_t next=0;
inline void Record(uint64_t address,uint64_t bytes,uint64_t offset,std::string path){
    if(!Enabled()||bytes<2048)return;
    address=Kernel::AddressFold::Fold(address);
    if(address>UINT64_MAX-bytes)return;
    std::scoped_lock lock{mutex};
    const auto id=++next;
    events[(id-1)%events.size()]={id,address,address+bytes,offset,std::move(path)};
}
inline std::vector<Event> Overlap(uint64_t address,uint64_t bytes){
    std::vector<Event> result;
    if(!Enabled()||!bytes)return result;
    address=Kernel::AddressFold::Fold(address);
    if(address>UINT64_MAX-bytes)return result;
    std::scoped_lock lock{mutex};
    for(uint64_t i=0;i<events.size()&&i<next&&result.size()<32;++i){
        const auto& e=events[(next-1-i)%events.size()];
        if(e.id&&e.begin<address+bytes&&e.end>address)result.push_back(e);
    }
    return result;
}
}
