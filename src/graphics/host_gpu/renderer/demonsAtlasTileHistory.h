#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace Libs::Graphics::DemonsAtlasTileHistory {
constexpr uint32_t Width=256, Height=256, Layers=360, Tile=16;
constexpr uint32_t TileCount=Layers*(Width/Tile)*(Height/Tile);
constexpr uint32_t Invalid=UINT32_MAX;
constexpr uint32_t Key(uint32_t x,uint32_t y,uint32_t layer) {
    return x<Width&&y<Height&&layer<Layers ?
        layer*(Width/Tile)*(Height/Tile)+(y/Tile)*(Width/Tile)+x/Tile : Invalid;
}
constexpr uint32_t IndexKey(uint32_t packed,const std::array<uint32_t,6>& srt) {
    const auto i=packed&0xffffffu;
    const auto x=i&srt[2], y=(i>>(srt[0]&31u))&srt[3], z=(i>>(srt[1]&31u))&srt[4];
    if(x>=Width/Tile||y>=Height/Tile||z>=Layers)return Invalid;
    return Key(x*Tile,y*Tile,z);
}
struct Record {
    int32_t frame=-1;
    uint64_t submit=0,version=0;
    uint32_t group_z=0,packed_index=0;
    std::array<uint32_t,14> coefficients{};
    bool ambiguous=false;
};
struct History {
    uint64_t owner=0,version=0;
    bool valid=false;
    std::vector<Record> tiles;
    void Invalidate(){valid=false;tiles.clear();}
    void Begin(uint64_t o,uint64_t v){
        // Unknown version gaps cannot be attributed to an older dispatch.
        if(!valid||owner!=o||v<version||v-version>2){
            tiles.assign(TileCount,Record{});
        }
        owner=o;version=v;valid=true;
    }
    void Put(uint32_t key,Record record){
        if(!valid||key>=tiles.size())return;
        auto& old=tiles[key];
        if(old.frame==record.frame&&old.submit==record.submit&&old.version==record.version){
            // Same submit can contain multiple dispatches, so never assert
            // a unique lane when more than one candidate addresses the tile.
            record.ambiguous=true;
        }
        old=record;
    }
    const Record* Find(uint64_t o,uint64_t v,uint32_t key) const {
        if(!valid||owner!=o||version!=v||key>=tiles.size()||tiles[key].frame<0)return nullptr;
        return &tiles[key];
    }
};
}
