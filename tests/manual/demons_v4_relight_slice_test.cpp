#include "graphics/shader/demonsV4LightTrace.h"
#include <cassert>
#include <set>
using namespace Libs::Graphics::DemonsV4LightTrace;
int main(){
    static_assert(RelightWords==466952);
    static_assert(RelightBytes<Bytes);
    static_assert(RelightCorner(1)==0&&RelightCorner(128)==7&&RelightCorner(0)==8&&RelightCorner(3)==8&&RelightCorner(256)==8);
    std::set<uint32_t> rays;
    for(uint32_t y=0;y<2;++y)for(uint32_t x=0;x<2;++x)for(uint32_t ly=0;ly<8;++ly)for(uint32_t lx=0;lx<8;++lx)
        assert(rays.insert(RelightRay(x,y,lx,ly)).second);
    assert(rays.size()==256&&*rays.rbegin()==255);
    for(std::size_t i=0;i<RelightSites.size();++i){
        assert(RelightSites[i].count<=24);
        assert(RelightOffset(i+1)-RelightOffset(i)==RelightRows(RelightSites[i])*RelightStride);
    }
}
