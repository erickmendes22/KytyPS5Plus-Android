#include "graphics/host_gpu/renderer/pipeline/descriptorPoolPolicy.h"
#include <cassert>
#include <cstdio>
#include <limits>

using Libs::Graphics::DescriptorPoolPolicy::Capacity;
using Libs::Graphics::DescriptorPoolPolicy::Counts;
using Libs::Graphics::DescriptorPoolPolicy::Fits;

int main() {
    constexpr Counts baseline{8192,4096,8192,1024,1024};
    constexpr Counts ordinary{16,12,32,64,8};
    constexpr Counts oversize{16,12,32,1200,8};
    constexpr Counts expanded{Capacity(baseline[0],oversize[0]),
                              Capacity(baseline[1],oversize[1]),
                              Capacity(baseline[2],oversize[2]),
                              Capacity(baseline[3],oversize[3]),
                              Capacity(baseline[4],oversize[4])};
    static_assert(Fits(baseline, ordinary));
    static_assert(!Fits(baseline, oversize));
    static_assert(Fits(expanded, oversize));
    static_assert(expanded[3] == 4800);
    static_assert(expanded[0] == baseline[0]); // avoid multiplying unrelated pools
    static_assert(Capacity(1024, 0) == 1024);
    static_assert(Capacity(1024, 256) == 1024);
    static_assert(Capacity(1024, 257) == 1028);
    static_assert(Capacity(1024, std::numeric_limits<uint32_t>::max()) ==
                  std::numeric_limits<uint32_t>::max());
    static_assert(!Fits(Counts{8192,4096,8192,1024,0}, ordinary)); // missing descriptor type
    std::puts("descriptor pool policy: all capacity and rollover constraints passed");
}
