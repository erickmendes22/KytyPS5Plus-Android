#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>
namespace Kernel::SarosHostBank {
constexpr uint64_t GiB=1ull<<30, FoldBoundary=0x8000000000ull,
    FoldMask=FoldBoundary-1, HostBegin=0x1000000000ull, HostEnd=0x5f00000000ull;
struct Span { uint64_t guest; uint64_t size; };
struct Placement { uint64_t shift, margin; };
// High fixed direct mappings captured on device. Constraints, not reservations.
constexpr std::array<Span,4> ObservedHighSpans {{{0x90c0000000ull,192ull<<20},
 {0x98c0000000ull,192ull<<20},{0x9cc0000000ull,192ull<<20},{0xcfc0000000ull,32ull<<20}}};
template<class PhysicalFreeMargin>
std::optional<Placement> Select(uint64_t guest,uint64_t size,PhysicalFreeMargin free) {
  if (guest<FoldBoundary || guest>=2*FoldBoundary || !size || size>2*FoldBoundary-guest) return {};
  std::optional<Placement> result;
  for(uint64_t shift=GiB;shift<=60*GiB;shift+=GiB) {
    uint64_t margin=UINT64_MAX; bool valid=true;
    auto check=[&](Span span) {
      if (!valid) return;
      const uint64_t offset=span.guest&FoldMask;
      if (offset>UINT64_MAX-shift) {valid=false;return;}
      const uint64_t host=offset+shift;
      if(host<HostBegin || span.size>HostEnd-HostBegin || host>HostEnd-span.size) {valid=false;return;}
      const uint64_t score=free(host,span.size);
      if (!score) {valid=false;return;}
      margin=std::min(margin,score);
    };
    check({guest,size});
    for(const auto span:ObservedHighSpans) check(span);
    if(valid && (!result||margin>result->margin)) result=Placement{shift,margin};
  }
  return result;
}
} // namespace Kernel::SarosHostBank
