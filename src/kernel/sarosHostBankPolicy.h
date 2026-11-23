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
// When the UE reservation selects a bank, the game's legitimate low guest
// fixed maps may NOT have been committed yet. Physical placeholder checks
// alone cannot predict future occupancy. These addresses were verified as
// distinct low-guest mappings in successive Saros crash logs:
//  0x1800000000 (bca598c), 0x18c0000000 (d122b78),
//  0x1cc0000000, 0x3c00000000 (preceding conflicts).
// Treat them as constraints, not host VM reservations. This keeps real
// low guest pages accessible and rejects a high bank that aliases them.
constexpr std::array<Span,4> ObservedLowSpans {{{
    0x1800000000ull,0x10000ull},{0x18c0000000ull,0x10000ull},
    {0x1cc0000000ull,0x10000ull},{0x3c00000000ull,0x10000ull}}};
constexpr bool Overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m) {
    return n && m && a < b+m && b < a+n;
}
// Find the end of a future high-guest host bank that overlaps a
// *non-fixed* low-guest allocation proposal. Returning this end allows the
// guest VA allocator to move to the next free range, instead of using a
// low guest address whose host backing would collide with a later high
// direct mapping. Fixed guest mappings MUST NOT be silently moved.
constexpr uint64_t LowAllocatorSkipEnd(uint64_t proposed,uint64_t bytes,uint64_t shift) {
  if (!shift || !bytes || proposed >= FoldBoundary ||
      proposed > UINT64_MAX-bytes || proposed+bytes > FoldBoundary) return 0;
  uint64_t end = 0;
  for(const auto high: ObservedHighSpans) {
    const uint64_t host=(high.guest & FoldMask)+shift;
    if (Overlaps(proposed,bytes,host,high.size))
      end=std::max(end,host+high.size);
  }
  return end;
}
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
      // A high bank that is free *today* is still wrong when a known,
      // not-yet-mapped low guest allocation will occupy it later.
      for (const auto low:ObservedLowSpans) {
        if (Overlaps(host,span.size,low.guest,low.size)) {
          valid=false;return;
        }
      }
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
