#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Libs::Graphics::DemonsIndirectWitnessPolicy {
constexpr uint64_t MetadataBudget = 8ull << 20;
constexpr uint64_t BlobBudget = 24ull << 20;
constexpr uint64_t GenerationBudget = 8ull << 20;
constexpr uint64_t RecordBytes = 20;
constexpr uint64_t BatchBytes = 60;
// Retain the changed producer and its next seven candidates even when the
// target is unchanged. Budget exhaustion is explicit, not evidence of a match.
constexpr size_t ProducerChainLimit = 8;
constexpr bool RetainCandidate(bool restart,size_t chain_size){return restart||chain_size<ProducerChainLimit;}
constexpr uint64_t ReplacementCapacity(uint64_t total,uint64_t old){
    if(old>total||total-old>BlobBudget)return 0;
    const auto free=BlobBudget-(total-old);return free<GenerationBudget?free:GenerationBudget;
}
constexpr bool MetadataFits(uint64_t batches,uint64_t events,uint64_t manifest){
    constexpr uint64_t cap=MetadataBudget-16384;
    return batches<=cap&&events<=cap-batches&&manifest<=cap-batches-events;
}
constexpr bool ValidRange(uint64_t address, uint64_t bytes) {
    return address && bytes && bytes <= UINT64_MAX - address;
}
constexpr bool Overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return ValidRange(a,n) && ValidRange(b,m) && a < b+m && b < a+n;
}
constexpr bool Shape(bool indexed, uint32_t count, uint32_t stride, uint32_t type,
                     uint32_t capacity, bool same_target) {
    return indexed && count==3 && stride==20 && type==1 && capacity==22020096 && same_target;
}
// A boot-time zero is not the transition. Candidates remain movable until the
// existing albedo-loss trigger seals them; an earlier valid zero cannot steal
// the real cycle merely because it happened first during loading.
struct Retention {
    uint64_t last_nonzero=0, first_zero=0;
    bool sealed=false;
    constexpr bool Observe(uint64_t batch, bool nonzero) {
        if(sealed)return false;
        if(nonzero){last_nonzero=batch;first_zero=0;return true;}
        if(last_nonzero&&!first_zero){first_zero=batch;return true;}
        return false;
    }
    constexpr void Seal(){sealed=true;}
};
struct CollectionBarrier {
    uint32_t required=3, consumed=0;
    constexpr void Consumed(uint32_t child){if(child<required&&child<32)consumed|=uint32_t{1}<<child;}
    constexpr bool Ready() const{return required>0&&required<32&&consumed==((uint32_t{1}<<required)-1);}
};
constexpr uint64_t Digest(std::span<const uint8_t> data) {
    uint64_t value=14695981039346656037ull;
    for(const auto byte:data){value^=byte;value*=1099511628211ull;}
    return value;
}
// Values are supplied by the original accesses, not by a preliminary memcpy.
// Repeated field loads remain visible; the final emitted tuple is separate.
struct Uses {
    struct Use { uint8_t field=0, phase=0; uint32_t value=0; };
    std::array<Use,48> entries{};
    std::array<uint32_t,5> words{};
    std::array<bool,5> seen{};
    size_t count=0;
    bool overflow=false;
    uint32_t Add(uint8_t field,uint8_t phase,uint32_t value) {
        if(field<5){words[field]=value;seen[field]=true;}
        if(count<entries.size())entries[count++]={field,phase,value};else overflow=true;
        return value;
    }
    bool Complete() const { for(bool value:seen)if(!value)return false;return !overflow; }
};
}
