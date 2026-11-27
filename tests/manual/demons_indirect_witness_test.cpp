#include "graphics/host_gpu/renderer/demonsIndirectWitnessPolicy.h"
#include "graphics/host_gpu/renderer/demonsP8CapturePolicy.h"
#include <cassert>
#include <limits>

using namespace Libs::Graphics::DemonsIndirectWitnessPolicy;
int main(){
    namespace P8=Libs::Graphics::DemonsP8CapturePolicy;
    static_assert(!P8::AttachWholeStateLayer(true,true,true));
    static_assert(!P8::AttachWholeStateLayer(true,false,false));
    static_assert(P8::RawCopyBudget<1536ull*1024*1024);
    static_assert(P8::RawStorageBudget<=P8::RawCopyBudget);
    static_assert(P8::Trigger(380,0.000041,0));
    static_assert(!P8::Trigger(239,0.0,0));
    static_assert(!P8::Trigger(380,0.01,0));
    static_assert(!P8::Trigger(380,0.000041,9));
    static_assert(P8::Reusable(false,false,true,true,true));
    P8::SustainedDark selection;
    assert(!selection.Observe(238,0.000041,0));
    assert(!selection.Observe(262,0.5831263,64));
    assert(!selection.Observe(310,0.04832,64));
    assert(!selection.Observe(334,0.0000367,0));
    assert(!selection.Observe(334,0.0000367,0)); // duplicate frame
    assert(!selection.Observe(358,0.0000368,0));
    assert(selection.Observe(382,0.0000365,0));
    static_assert(P8::ProducerFrame(383,384,false,false));
    static_assert(P8::ProducerFrame(384,384,false,false));
    static_assert(!P8::ProducerFrame(382,384,false,false));
    static_assert(!P8::ProducerFrame(385,384,false,false));
    static_assert(!P8::ProducerFrame(384,384,true,false));
    static_assert(!P8::ProducerFrame(383,384,false,true));
    static_assert(!P8::Reusable(true,false,true,true,true));
    static_assert(!P8::Reusable(false,true,true,true,true));
    static_assert(!P8::Reusable(false,false,false,true,true));
    static_assert(!P8::Reusable(false,false,true,false,true));
    static_assert(!P8::Reusable(false,false,true,true,false));
    static_assert(Shape(true,3,20,1,22020096,true));
    static_assert(!Shape(true,3,20,0,22020096,true));
    static_assert(!Shape(true,3,20,1,22020095,true));
    static_assert(!Shape(true,3,20,1,22020096,false));
    static_assert(!Shape(false,3,20,1,22020096,true));
    static_assert(Overlap(0x21de00078,60,0x21de000a0,20));
    static_assert(!Overlap(0x21de00078,60,0x21de000b4,20));
    static_assert(!ValidRange(UINT64_MAX-20,60));
    Retention retention;
    assert(!retention.Observe(1,false)); // boot/loading zero is not the edge
    assert(retention.Observe(2,true)&&retention.last_nonzero==2);
    assert(retention.Observe(3,true)&&retention.last_nonzero==3);
    assert(retention.Observe(4,false)&&retention.first_zero==4);
    assert(!retention.Observe(5,false));
    assert(retention.Observe(6,true)); // old valid zero is superseded
    assert(retention.last_nonzero==6&&retention.first_zero==0);
    assert(retention.Observe(7,false));
    retention.Seal(); // existing albedo population loss trigger
    assert(!retention.Observe(8,true));
    assert(retention.last_nonzero==6&&retention.first_zero==7);

    // A diagnostic wait publishes stale backing. If it runs after child zero,
    // it changes the values originally read by the other two children.
    std::array<uint32_t,3> memory{98304,17,29},observed{},frozen{};
    CollectionBarrier barrier;
    unsigned waits=0;
    const auto collect=[&]{assert(barrier.Ready());++waits;memory[1]=0;memory[2]=0;};
    assert(!barrier.Ready());
    for(uint32_t child=0;child<3;++child){
        observed[child]=memory[child]; // original semantic load
        frozen[child]=memory[child]; // queued copy has a separate owner
        barrier.Consumed(child);
        assert(barrier.Ready()==(child==2));
        assert(waits==0);
    }
    collect();
    assert(waits==1&&observed==frozen&&observed[1]==17&&observed[2]==29);
    assert(memory[1]==0&&memory[2]==0); // collect actually exercises publication
    CollectionBarrier wrong;
    wrong.Consumed(0);wrong.Consumed(0);wrong.Consumed(2);wrong.Consumed(3);
    assert(!wrong.Ready()); // duplicate/out-of-range children cannot unlock it

    Uses uses;
    uint32_t changing=98304;
    assert(uses.Add(0,0,changing)==98304);
    changing=0;
    assert(uses.Add(0,3,changing)==0); // repeated original accesses are retained
    assert(uses.entries[0].value==98304&&uses.entries[1].value==0);
    assert(!uses.Complete());
    for(uint8_t field=1;field<5;++field)uses.Add(field,6,field);
    assert(uses.Complete()&&uses.words[0]==0);
    for(unsigned i=0;i<64;++i)uses.Add(0,3,i);
    assert(uses.overflow&&!uses.Complete());

    static_assert(RetainCandidate(false,1));
    static_assert(RetainCandidate(false,ProducerChainLimit-1));
    static_assert(!RetainCandidate(false,ProducerChainLimit));
    static_assert(RetainCandidate(true,ProducerChainLimit));
    static_assert(ReplacementCapacity(64ull<<20,16ull<<20)==(16ull<<20));
    static_assert(ReplacementCapacity(64ull<<20,0)==0);
    static_assert(ImagePayloadFits(12,44,2,4,4));
    static_assert(!ImagePayloadFits(12,43,2,4,4));
    static_assert(!ImagePayloadFits(45,44,1,1,4));
    static_assert(!ImagePayloadFits(0,UINT64_MAX,UINT32_MAX,UINT32_MAX,4));
    static_assert(!ImagePayloadFits(0,64,0,1,4));
    static_assert(ReplacementCapacity(0,1)==0);
    static_assert(MetadataFits(4ull<<20,3ull<<20,512ull<<10));
    static_assert(!MetadataFits(4ull<<20,4ull<<20,1));
    static_assert(!MetadataFits(UINT64_MAX,1,1));
    constexpr std::array<uint8_t,5> hello{'h','e','l','l','o'};
    static_assert(Digest(hello)==0xa430d84680aabd0bull);
    auto changed=hello;changed[0]='H';assert(Digest(changed)!=Digest(hello));
}
