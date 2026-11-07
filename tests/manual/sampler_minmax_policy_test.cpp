#include "graphics/host_gpu/renderer/cache/samplerReductionPolicy.h"
#include <cassert>
#include <cstdio>
using Libs::Graphics::UseSamplerMinMaxReduction;
using Libs::Graphics::SamplerComparisonEnabled;
int main() {
    assert(SamplerComparisonEnabled(true, false));
    assert(!SamplerComparisonEnabled(false, false));
    assert(!SamplerComparisonEnabled(true, true));
    for (unsigned m = 0; m <= 3; ++m) {
        assert(!UseSamplerMinMaxReduction(false, false, m));
        assert(!UseSamplerMinMaxReduction(true, true, m));
        assert(UseSamplerMinMaxReduction(true, false, m) == (m == 1 || m == 2));
    }
    assert(!UseSamplerMinMaxReduction(true, false, 4));
    std::puts("Sampler reduction: 0 default, min/max require feature and no comparison");
}
