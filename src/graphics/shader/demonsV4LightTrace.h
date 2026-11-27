#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
namespace Libs::Graphics::DemonsV4LightTrace {
constexpr uint64_t Shader=0x3dfe402653925fbfull;
constexpr uint32_t Header=8,Lanes=64,FixedStride=96,FixedBase=Header;
constexpr uint32_t OuterStride=8,OuterBase=FixedBase+Lanes*FixedStride;
constexpr uint32_t Terms=1024,TermStride=24,TermBase=OuterBase+32*Lanes*OuterStride;
constexpr uint32_t ImageBase=TermBase+(1+2*Terms)*TermStride,ImageStride=28;
struct Site {uint32_t pc,loop,coord_count,dst,count,desc,sampler;std::array<uint32_t,7> coords;};
constexpr std::array<Site,21> Images{{
 {0x118,0,2,1,1,4,255,{4,5}}, {0x150,0,2,9,1,20,255,{4,5}},
 {0x164,0,3,0,1,4,255,{4,5,0}}, {0x170,0,3,13,1,4,255,{4,5,7}},
 {0x17c,0,3,34,1,4,255,{4,5,2}}, {0x1e8,0,2,55,4,12,52,{6,7}},
 {0x364,0,2,6,2,36,255,{26,27}}, {0x57c,0,7,0,3,0,52,{47,48,49,50,51,52,53}},
 {0x5c4,0,2,0,2,8,255,{4,5}}, {0xa70,1,7,0,3,0,52,{51,50,52,53,2,1,0}},
 {0x1010,2,7,0,3,0,52,{51,53,52,54,1,0,2}}, {0x1328,2,7,0,3,0,52,{55,57,56,58,54,2,0}},
 {0x1410,2,4,56,1,16,44,{2,56,57,0}}, {0x1424,2,4,0,1,16,44,{2,55,57,0}},
 {0x147c,2,4,56,1,16,44,{2,56,57,55}}, {0x1488,2,4,1,1,16,44,{2,54,1,55}},
 {0x1864,2,4,53,1,16,44,{55,53,54,2}}, {0x1874,2,4,2,1,16,44,{55,52,54,2}},
 {0x18e8,2,4,50,1,16,44,{55,56,57,58}}, {0x18f0,2,4,0,1,16,44,{55,0,1,58}},
 {0x1b9c,2,3,0,3,92,52,{57,58,59}}
}};
constexpr uint32_t ImageOffset(std::size_t site){uint32_t off=ImageBase;for(std::size_t i=0;i<site;++i)off+=(Images[i].loop?Terms:1)*ImageStride;return off;}
constexpr uint32_t Words=ImageOffset(Images.size());
constexpr uint64_t Bytes=uint64_t(Words)*4;
static_assert(Bytes<8*1024*1024);
inline bool Enabled(){const auto* e=std::getenv("KYTY_DS_MATRIX");return e&&e[0]=='8'&&e[1]=='\0';}
inline std::atomic<uint64_t> address{0};
inline uint64_t Address(){return address.load(std::memory_order_relaxed);}
inline void SetAddress(uint64_t a){address.store(a,std::memory_order_relaxed);}
inline bool Supports(uint32_t x,uint32_t y,uint32_t z,uint32_t wave){return x==8&&y==8&&z==1&&wave==64;}
}
