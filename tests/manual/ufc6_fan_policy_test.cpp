#include "graphics/host_gpu/renderer/pipeline/fanAutoPolicy.h"
#include <cassert>
#include <cstdint>
using Libs::Graphics::WalkFan::FanAutoPolicy;
using Libs::Graphics::WalkFan::FanAutoConfig;

int main() {
    FanAutoConfig config{};
    config.engage_us = 40.0;
    config.disengage_us = 18.0;
    FanAutoPolicy policy(config, true);
    (void)policy.Update(0, 0, 0);
    // Initially on, 5.5us/walk crosses the off threshold after 6s dwell.
    auto a = policy.Update(6'000'000'000ll, 5'500'000ull, 1'000);
    assert(a.switched && !a.engaged);
    // 34.4us/walk must not re-enable two-stage fan+pipeline: 34.4 < 40.
    a = policy.Update(12'000'000'000ll, 39'900'000ull, 2'000);
    assert(!a.switched && !a.engaged);
    // Sustained >40us is the only permitted re-engage after dwell.
    a = policy.Update(18'000'000'000ll, 84'900'000ull, 3'000);
    assert(a.switched && a.engaged);
    // 14.7us/walk is cheap enough to disengage after the next dwell.
    a = policy.Update(24'000'000'000ll, 99'600'000ull, 4'000);
    assert(a.switched && !a.engaged);
}
