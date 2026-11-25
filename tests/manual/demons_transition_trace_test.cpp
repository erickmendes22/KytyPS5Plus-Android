#include "graphics/host_gpu/renderer/demonsTransitionTrace.h"
#include <cassert>
using namespace Libs::Graphics::DemonsTransitionTrace;
int main(){
    std::array<uint32_t,32> rows{};std::array<uint32_t,2> ids{11,12};
    rows[1]=8;rows[13]=2;rows[14]=11;rows[30]=12;
    auto c=Summarize(rows,ids);assert(c.valid==2&&c.group1==1&&c.group1_lights==1&&!c.inconsistent);
    auto copy=rows;assert(ExactWriter(rows,copy));copy[1]=0;assert(!ExactWriter(rows,copy));
    rows[1]=0;assert(Summarize(rows,ids).inconsistent==1);
    rows[14]=12;assert(Summarize(rows,ids).valid==1);
    assert(Descriptor224Address(Pointer(0x12340000,3))==0x312340c30ull);
    assert(!Descriptor224Address(0)&&!Descriptor224Address(UINT64_MAX));
}
