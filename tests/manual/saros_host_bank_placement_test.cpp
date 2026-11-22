#include "kernel/sarosHostBankPolicy.h"
#include <algorithm>
#include <array>
#include <cassert>
using namespace Kernel::SarosHostBank;
constexpr bool Overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return a<b+m&&b<a+n;}
int main(){
  constexpr std::array<Span,3> low {{{0x18c0000000ull,65536},{0x1cc0000000ull,65536},{0x3c00000000ull,65536}}};
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
  assert(p && p->shift!=24*GiB && p->shift!=32*GiB);
  for(const auto high:ObservedHighSpans) {
    const auto host=(high.guest&FoldMask)+p->shift;
    assert(host>=HostBegin&&host+high.size<=HostEnd);
    for(const auto l:low)assert(!Overlap(host,high.size,l.guest,l.size));
  }
  assert(!Select(0x90c0000000ull,192ull<<20,[](uint64_t,uint64_t)->uint64_t{return 0;}));
}
