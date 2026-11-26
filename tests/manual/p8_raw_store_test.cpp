#include "graphics/host_gpu/renderer/demonsP8RawStore.h"
#include <cassert>
#include <filesystem>
#include <array>
using Libs::Graphics::DemonsP8Reference::RawStore;
int main(){
    auto dir=std::filesystem::temp_directory_path()/"kyty-p8-raw-store-test";
    std::filesystem::remove_all(dir);std::filesystem::create_directories(dir);
    RawStore store(8,20);
    std::array<uint8_t,4> a{1,2,3,4},b{4,3,2,1},c{0,0,0,0};
    auto id1=store.ReserveCopy(a.size());
    auto id2=store.ReserveCopy(a.size());
    assert(id1&&id2&&id1!=id2);
    auto x=store.Save(dir,id1,a);
    auto y=store.Save(dir,id2,a);
    assert(!x.reason&&!x.reused&&!y.reason&&y.reused&&x.file==y.file);
    auto z=store.Save(dir,store.ReserveCopy(b.size()),b);
    assert(!z.reason&&!z.reused&&z.file!=x.file);
    auto reject=store.Save(dir,store.ReserveCopy(c.size()),c);
    assert(reject.reason&&reject.file.empty());
    // Same content can still be retained when the unique-byte budget is full.
    auto last=store.Save(dir,store.ReserveCopy(a.size()),a);
    assert(last.reused&&last.file==x.file);
    assert(!store.ReserveCopy(1));
    // A missing/corrupt original is never accepted on digest alone.
    RawStore check(8,8);
    auto q=check.Save(dir,check.ReserveCopy(a.size()),a);
    std::ofstream(dir/q.file,std::ios::binary|std::ios::trunc)<<"xxxx";
    auto repair=check.Save(dir,check.ReserveCopy(a.size()),a);
    assert(!repair.reason&&!repair.reused&&repair.file!=q.file);
    std::filesystem::remove_all(dir);
}
