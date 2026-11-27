#include "graphics/host_gpu/renderer/demonsP8RawStore.h"
#include "graphics/host_gpu/renderer/demonsP8CapturePolicy.h"
#include <cassert>
#include <filesystem>
#include <array>
using Libs::Graphics::DemonsP8Reference::RawStore;
using namespace Libs::Graphics::DemonsP8CapturePolicy;
int main(){
    static_assert(CriticalRole("compositor_overlay_input"));
    static_assert(CriticalRole("compositor_scene_input"));
    static_assert(CriticalRole("hzb_output"));
    static_assert(CaptureCriticalFrame(true,false,"hzb_input"));
    static_assert(!CaptureCriticalFrame(true,false,"compositor"));
    static_assert(!CaptureCriticalFrame(false,false,"hzb_output"));
    static_assert(std::string_view(RadianceInputRole(CompositePS,0x281a80000ull,true,true,false,1,1,16711680))=="compositor_overlay_input");
    static_assert(std::string_view(RadianceInputRole(CompositePS,SceneOwner,true,true,false,1,1,12779520))=="compositor_scene_input");
    static_assert(!RadianceInputRole(CompositePS,SceneOwner,true,false,true,1,1,12779520));
    auto dir=std::filesystem::temp_directory_path()/"kyty-p8-paired-budget";
    std::filesystem::remove_all(dir);std::filesystem::create_directories(dir);
    RawStore store(4,8,8,8);
    std::array<uint8_t,4>a{1,2,3,4},b{5,6,7,8},c{9,10,11,12};
    auto x=store.ReserveCopy(4);assert(x);
    assert(!store.Save(dir,x,a).reason);
    auto y=store.ReserveCopy(4);assert(y);
    assert(store.Save(dir,y,b).reason);
    assert(!store.ReserveCopy(1));
    auto z=store.ReserveCopy(4,true);assert(z&&z!=x&&z!=y);
    assert(!store.Save(dir,z,b,true).reason);
    auto q=store.ReserveCopy(4,true);assert(q&&q!=z);
    assert(!store.Save(dir,q,c,true).reason);
    assert(!store.ReserveCopy(1,true));
    std::filesystem::remove_all(dir);
}
