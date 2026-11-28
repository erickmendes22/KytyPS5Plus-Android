#include "graphics/shader/demonsV4LightTrace.h"
#include <cassert>
#include <set>
using namespace Libs::Graphics::DemonsV4LightTrace;
int main(){
    static_assert(RelightWords==598024);
    static_assert(RelightBytes<Bytes);
    static_assert(RelightCorner(1)==0&&RelightCorner(128)==7&&RelightCorner(0)==8&&RelightCorner(3)==8&&RelightCorner(256)==8);
    constexpr std::array<uint32_t,4> other{0,0,0,0};
    constexpr std::array<std::array<uint32_t,4>,3> positions{other,RelightPosition,other};
    constexpr std::array<uint64_t,3> hashes{0,RelightReferenceRayHash,0};
    static_assert(RelightChoose(positions,hashes)==1);
    constexpr std::array<std::array<uint32_t,4>,3> duplicate{RelightPosition,RelightPosition,other};
    static_assert(RelightChoose(duplicate,hashes)==1);
    constexpr std::array<uint64_t,3> ambiguous{RelightReferenceRayHash,RelightReferenceRayHash,0};
    static_assert(RelightChoose(duplicate,ambiguous)==RelightAmbiguous);
    constexpr std::array<uint64_t,3> changed{0,123,0};
    static_assert(RelightChoose(positions,changed)==1);
    static_assert(RelightChoose(duplicate,changed)==RelightAmbiguous);
    static_assert(RelightChoose(std::span{positions}.first(1),std::span{hashes}.first(1))==RelightNoMatch);
    std::set<uint32_t> rays;
    for(uint32_t y=0;y<2;++y)for(uint32_t x=0;x<2;++x)for(uint32_t ly=0;ly<8;++ly)for(uint32_t lx=0;lx<8;++lx)
        assert(rays.insert(RelightRay(x,y,lx,ly)).second);
    assert(rays.size()==256&&*rays.rbegin()==255);
    for(std::size_t i=0;i<RelightSites.size();++i){
        assert(RelightSites[i].count<=24);
        assert(RelightOffset(i+1)-RelightOffset(i)==RelightRows(RelightSites[i])*RelightStride);
    }
}
