#include "graphics/host_gpu/renderer/pipeline/demonsCountArithmeticProbe.h"
#include <cassert>
using namespace Libs::Graphics::DemonsCountArithmeticProbe;
int main(){
    static_assert(Layout(Three).ids[SecValue]==66&&Layout(Three).ids[StoreSecondary]==159);
    static_assert(Layout(Two).ids[SecValue]==47&&Layout(Two).ids[StoreSecondary]==142);
    static_assert(Layout(Two).ids[SecLoad]==0&&WordCount==43);
    std::vector<uint32_t> bad{0x07230203,0x00010300,0,175,0};
    auto original=bad;
    assert(!Instrument(bad,Three,55)&&bad==original);
    assert(!Instrument(bad,0,55)&&bad==original);
}
