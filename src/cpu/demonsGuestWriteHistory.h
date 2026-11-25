#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "kernel/addressFold.h"

namespace Cpu::DemonsGuestWriteHistory {
// Candidate first writes to GPU-watched pages, not an instruction/store trace.
// The 64 KiB window includes the usual Android fault-window optimization;
// unprotected pages may be written again without any fault.
constexpr uint64_t Window=64ull*1024;
constexpr size_t Capacity=32768;
struct Event {uint64_t id=0,address=0,rip=0,rsi=0,rdi=0,rcx=0;};
struct Cell {std::atomic<uint64_t> seq{},address{},rip{},rsi{},rdi{},rcx{};};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
inline std::array<Cell,Capacity> ring{};
inline std::atomic<uint64_t> next{};
inline std::atomic<bool> enabled{};
inline void Init(){
    const char* v=std::getenv("KYTY_DS_RAY_SOURCE_HISTORY");
    enabled.store(v&&v[0]=='1'&&v[1]=='\0',std::memory_order_release);
}
inline bool Enabled(){return enabled.load(std::memory_order_acquire);}
inline void Record(uint64_t address,uint64_t rip,uint64_t rsi,uint64_t rdi,uint64_t rcx) noexcept{
    if(!rip)return;
    const auto id=next.fetch_add(1,std::memory_order_relaxed)+1;
    auto& c=ring[(id-1)%Capacity];
    c.seq.store(0,std::memory_order_release);
    c.address.store(address,std::memory_order_relaxed);
    c.rip.store(rip,std::memory_order_relaxed);c.rsi.store(rsi,std::memory_order_relaxed);
    c.rdi.store(rdi,std::memory_order_relaxed);c.rcx.store(rcx,std::memory_order_relaxed);
    c.seq.store(id,std::memory_order_release);
}
struct Result {std::vector<Event> events;bool truncated=false;};
inline Result Overlap(uint64_t address,uint64_t size){
    Result result;if(!size)return result;
    address=Kernel::AddressFold::Fold(address);
    if(size>UINT64_MAX-address)return result;
    const auto n=next.load(std::memory_order_acquire);
    for(uint64_t i=0;i<n&&i<Capacity;++i){
        const auto id=n-i;const auto& c=ring[(id-1)%Capacity];
        if(c.seq.load(std::memory_order_acquire)!=id)continue;
        Event e{id,c.address.load(std::memory_order_relaxed),c.rip.load(std::memory_order_relaxed),
            c.rsi.load(std::memory_order_relaxed),c.rdi.load(std::memory_order_relaxed),c.rcx.load(std::memory_order_relaxed)};
        if(c.seq.load(std::memory_order_acquire)!=id)continue;
        const auto begin=e.address&~(Window-1);
        if(begin<address+size&&begin<=UINT64_MAX-Window&&begin+Window>address){
            if(result.events.size()==2048){result.truncated=true;break;}
            result.events.push_back(e);
        }
    }
    return result;
}
}
