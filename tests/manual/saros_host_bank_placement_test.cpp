#include "kernel/sarosHostBankPolicy.h"
#include <algorithm>
#include <array>
#include <cassert>
using namespace Kernel::SarosHostBank;
constexpr bool Overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return a<b+m&&b<a+n;}
int main(){
  constexpr auto low = ObservedLowSpans;
  const auto free=[&](uint64_t h,uint64_t n)->uint64_t{
    for(const auto s:low)if(Overlap(h,n,s.guest,s.size))return 0;
    uint64_t score=std::min(h-HostBegin,HostEnd-(h+n))+1;
    for(const auto s:low){
      const uint64_t d=h>=s.guest+s.size?h-(s.guest+s.size):s.guest>=h+n?s.guest-(h+n):0;
      score=std::min(score,d+1);
    }
    return score;
  };
  const auto p=Select(0x90c0000000ull,192ull<<20,free);
  assert(p && p->shift!=29*GiB && p->shift!=32*GiB);
  // A 24 GiB candidate is allowed again: its prior failure was an
  // invalid FEX IR constant operand, not a demonstrated host collision.
  // Compatibility must be determined by actual guest spans, not a list
  // of shifts forbidden because an earlier build failed elsewhere.
  for(const auto high:ObservedHighSpans) {
    const auto host=(high.guest&FoldMask)+p->shift;
    assert(host>=HostBegin&&host+high.size<=HostEnd);
    for(const auto l:low)assert(!Overlap(host,high.size,l.guest,l.size));
  }
  assert(!Select(0x90c0000000ull,192ull<<20,[](uint64_t,uint64_t)->uint64_t{return 0;}));
  // Regression: first bank selected when the future low guest mapping is
  // not yet present in m_free, then a nonfixed guest allocation arrives.
  // The kernel VA finder must advance past the entire high host view.
  const auto shift=28*GiB;
  const auto high0=(ObservedHighSpans[0].guest&FoldMask)+shift;
  assert(high0==0x17c0000000ull);
  assert(LowAllocatorSkipEnd(high0,0x30000ull,shift)==high0+(192ull<<20));
  assert(LowAllocatorSkipEnd(high0-0x4000ull,0x10000ull,shift)==high0+(192ull<<20));
  assert(LowAllocatorSkipEnd(high0+(192ull<<20),0x10000ull,shift)==0);
  assert(LowAllocatorSkipEnd(0x17c0000000ull,0x30000ull,0)==0);
  // Device regression: a late fixed low map consumes the exact beginning
  // of the provisional bank before the first real high map requests backing.
  const auto late_low_map = [&](uint64_t host, uint64_t bytes) -> uint64_t {
    return Overlap(host, bytes, high0, 0x30000ull) ? 0 : free(host, bytes);
  };
  assert(IsFreeForFirstMap(shift, ObservedHighSpans[0].guest, ObservedHighSpans[0].size, free));
  assert(!IsFreeForFirstMap(shift, ObservedHighSpans[0].guest, ObservedHighSpans[0].size, late_low_map));
  const auto replacement = Select(ObservedHighSpans[0].guest, ObservedHighSpans[0].size, late_low_map);
  assert(replacement && replacement->shift != shift);
  assert(IsFreeForFirstMap(replacement->shift, ObservedHighSpans[0].guest,
                           ObservedHighSpans[0].size, late_low_map));
  // Even if no currently allocated host page conflicts, the future
  // low-guest 0x1800000000 map MUST forbid a 29 GiB high-bank choice.
  const auto empty_host=[](uint64_t,uint64_t)->uint64_t{return 1;};
  const auto early = Select(0x90c0000000ull,192ull<<20,empty_host);
  assert(early && early->shift != 29*GiB);
  for(const auto high:ObservedHighSpans) {
    const uint64_t h=(high.guest&FoldMask)+early->shift;
    for(const auto low_span:ObservedLowSpans)
      assert(!Overlaps(h,high.size,low_span.guest,low_span.size));
  }
}
