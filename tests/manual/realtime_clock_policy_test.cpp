#include "graphics/shader/realtimeClockPolicy.h"
#include <cassert>
#include <limits>
int main() {
    using namespace Libs::Graphics::RealtimeClock;
    assert(PeriodPs(true, true, 1.0f) == 1000);
    assert(PeriodPs(true, true, 10.0f) == 10000);
    assert(PeriodPs(true, true, 1000.0f / 19.2f) == 52083);
    assert(!PeriodPs(false, true, 1.0f) && !PeriodPs(true, false, 1.0f));
    assert(!PeriodPs(true, true, 0.0f) && !PeriodPs(true, true, -1.0f));
    assert(!PeriodPs(true, true, std::numeric_limits<float>::infinity()));
    assert(!PeriodPs(true, true, std::numeric_limits<float>::quiet_NaN()));
    assert(!PeriodPs(true, true, std::numeric_limits<float>::max()));
    assert(Shift(1000) == 3 && Shift(10000) == 0 && Shift(52083) == 0 && Shift(0) == 0);
}
