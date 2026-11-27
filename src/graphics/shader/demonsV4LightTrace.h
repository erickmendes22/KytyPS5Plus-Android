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
constexpr uint32_t LegacyWords=ImageOffset(Images.size());
// One secondary-light record per guest s91 index, at the actual consumed PCs.
constexpr uint32_t RecordBase=LegacyWords,RecordStride=48;
constexpr uint32_t SingleWords=RecordBase+Terms*RecordStride;
// Shared per-wave fields, disjoint per-witness term/sample/light-record tails.
constexpr uint32_t WitnessStride=SingleWords-TermBase;
constexpr uint32_t PairWords=SingleWords+WitnessStride;
constexpr uint32_t LightRecordBytes=0x180;
constexpr uint32_t LightRecordCount=224;
// Schema 4: append a bounded per-light, per-witness BRDF slice, without
// moving schema-3 wave, terms, image or light-record offsets.
struct BrdfOp {uint32_t pc,dst;};
constexpr std::array<uint32_t,37> BrdfLive{{12,13,14,18,19,20,21,22,26,27,28,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,48,49,50,51,52,53,54,55,60,61,62}};
constexpr std::array<BrdfOp,79> BrdfOps{{
 {0x1bb4,0}, {0x1bc0,55}, {0x1bc8,2}, {0x1bcc,54}, {0x1bd4,1}, {0x1bd8,53}, {0x1be0,0}, {0x1be4,57}, {0x1be8,52}, {0x1bec,57}, {0x1bf0,52}, {0x1bf4,57}, {0x1bf8,52}, {0x1bfc,56}, {0x1c00,54}, {0x1c04,52}, {0x1c08,53}, {0x1c0c,56}, {0x1c10,55}, {0x1c14,2}, {0x1c18,56}, {0x1c1c,55}, {0x1c20,2}, {0x1c24,56}, {0x1c2c,55}, {0x1c30,1}, {0x1c38,2}, {0x1c40,52}, {0x1c44,0}, {0x1c4c,54}, {0x1c54,2}, {0x1c5c,52}, {0x1c64,53}, {0x1c6c,52}, {0x1c70,48}, {0x1c74,53}, {0x1c7c,2}, {0x1c80,52}, {0x1c84,52}, {0x1c8c,52}, {0x1c90,48}, {0x1c94,52}, {0x1c9c,1}, {0x1ca4,53}, {0x1ca8,1}, {0x1cb0,53}, {0x1cb4,1}, {0x1cb8,48}, {0x1cbc,48}, {0x1cc0,2}, {0x1cc4,48}, {0x1ccc,2}, {0x1cd0,52}, {0x1cd8,2}, {0x1cdc,48}, {0x1ce0,0}, {0x1ce4,54}, {0x1ce8,52}, {0x1cf0,53}, {0x1cf8,51}, {0x1d00,2}, {0x1d04,52}, {0x1d08,54}, {0x1d0c,48}, {0x1d10,50}, {0x1d14,54}, {0x1d18,53}, {0x1d1c,51}, {0x1d20,50}, {0x1d24,52}, {0x1d28,1}, {0x1d2c,53}, {0x1d34,49}, {0x1d38,0}, {0x1d3c,51}, {0x1d40,52}, {0x1d48,0}, {0x1d4c,50}, {0x1d50,1}
}};
constexpr uint32_t BrdfScalar=1+BrdfLive.size(),BrdfResults=BrdfScalar+2;
constexpr uint32_t BrdfStride=BrdfResults+2*BrdfOps.size();
constexpr uint32_t BrdfWitnessWords=LightRecordCount*BrdfStride;
constexpr uint32_t Words=PairWords+2*BrdfWitnessWords;
static_assert(BrdfStride==198 && Words*4==3986112);
constexpr uint64_t LightBufferBytes=uint64_t(LightRecordBytes)*LightRecordCount;
constexpr uint64_t Bytes=uint64_t(Words)*4;
static_assert(Bytes<8*1024*1024);

constexpr uint64_t GiShader=0xe3daeb1e7426761bull;
constexpr uint32_t GiWaveBase=Header,GiWaveStride=12,GiSiteBase=GiWaveBase+Lanes*GiWaveStride;
struct GiSite {uint32_t pc,slot,kind,loop,coord_count,dst,count,desc,sampler;std::array<uint32_t,4> coords;};
constexpr std::array<GiSite,23> GiSites{{
 {0x104,0,0,0,2,0,1,4,255,{6,7}},
 {0x13c,1,0,0,2,11,1,4,255,{6,7}},
 {0x178,2,0,0,3,3,1,20,255,{6,7,1}},
 {0x184,2,0,0,3,10,1,20,255,{6,7,2}},
 {0x1a0,2,0,0,3,2,1,20,255,{6,7,1}},
 {0x3e4,2,0,0,3,12,1,20,255,{6,7,8}},
 {0x508,3,1,0,2,0,3,0,16,{27,28}},
 {0x5a4,4,0,0,3,21,1,0,255,{22,23,24}},
 {0x600,5,0,0,3,21,1,0,255,{0,1,2}},
 {0x618,2,2,0,1,0,3,28,255,{21}},
 {0x708,3,2,0,1,0,1,36,255,{21}},
 {0x9ec,4,2,1,1,5,1,24,255,{31}},
 {0xac0,6,1,1,4,33,1,8,32,{3,33,35,5}},
 {0xacc,7,1,1,3,3,3,0,16,{4,34,5}},
 {0xb44,5,2,0,1,25,1,40,255,{21}},
 {0xca4,2,2,0,1,0,2,28,255,{2}},
 {0xd58,2,2,0,1,0,1,28,255,{2}},
 {0xd74,3,2,0,1,0,1,36,255,{2}},
 {0xfa8,4,2,2,1,2,1,24,255,{30}},
 {0x107c,6,1,2,4,32,1,8,32,{0,32,34,2}},
 {0x1088,7,1,2,3,0,3,0,16,{1,33,2}},
 {0x112c,8,0,0,2,0,3,0,255,{6,7}},
 {0x1174,9,3,0,2,0,4,0,255,{6,7}}
}};
// Dynamic corner key is the original one-hot scalar mask, not an ordinal.
constexpr uint32_t GiRows(const GiSite& s){return s.loop?129:1;}
constexpr uint32_t GiOffset(std::size_t i){uint32_t n=0;for(std::size_t j=0;j<i;++j)n+=GiRows(GiSites[j])*ImageStride;return n;}
constexpr uint32_t GiWitnessWords=GiOffset(GiSites.size()),GiWords=GiSiteBase+2*GiWitnessWords;
constexpr uint64_t GiBytes=uint64_t(GiWords)*4;
static_assert(GiBytes<Bytes);
constexpr bool GiKey(uint32_t key){return key&&key<=128&&(key&(key-1))==0;}

inline bool Enabled(){const auto* e=std::getenv("KYTY_DS_MATRIX");return e&&e[0]=='8'&&e[1]=='\0';}
inline std::atomic<uint64_t> address{0};
inline uint64_t Address(){return address.load(std::memory_order_relaxed);}
inline void SetAddress(uint64_t a){address.store(a,std::memory_order_relaxed);}
inline bool Supports(uint32_t x,uint32_t y,uint32_t z,uint32_t wave){return x==8&&y==8&&z==1&&wave==64;}
}
