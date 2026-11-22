#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
namespace Libs::Graphics {
struct MaterialFullFrameDiff {
 bool valid=false;
 uint64_t changed=0,colored_before=0,colored_after=0;
 uint32_t min_x=0,min_y=0,max_x=0,max_y=0;
};
// 8x8 samples cannot distinguish a missed draw from a draw elsewhere. Scan
// the whole native R32_UINT attachment, keeping only bounded summary stats.
inline MaterialFullFrameDiff CompareMaterialFullFrame(
  std::span<const uint32_t> before,std::span<const uint32_t> after,
  uint32_t width,uint32_t height) {
 MaterialFullFrameDiff d {};
 const uint64_t area=uint64_t{width}*height;
 if(!width||!height||area>4ull*1024*1024||before.size()!=area||after.size()!=area)return d;
 d.valid=true;
 for(uint64_t i=0;i<area;++i){
  d.colored_before+=(before[i]&0xffffffu)!=0;
  d.colored_after+=(after[i]&0xffffffu)!=0;
  if(before[i]!=after[i]) {
   const uint32_t x=static_cast<uint32_t>(i%width),y=static_cast<uint32_t>(i/width);
   if(!d.changed) {d.min_x=d.max_x=x;d.min_y=d.max_y=y;}
   else {
    d.min_x=std::min(d.min_x,x);d.max_x=std::max(d.max_x,x);
    d.min_y=std::min(d.min_y,y);d.max_y=std::max(d.max_y,y);
   }
   ++d.changed;
  }
 }
 return d;
}
} // namespace Libs::Graphics
