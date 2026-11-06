#include "cpu/guestTscPolicy.h"
#include <cassert>
#include <cstdio>
int main() {
    using Cpu::GuestTscPolicy::Scale;
    static_assert(Scale(19'200'000, false) == 6);
    static_assert(Scale(24'000'000, false) == 6);
    static_assert(Scale(100'000'000, false) == 4);
    static_assert(Scale(1'000'000'000, false) == 0);
    static_assert(Scale(0, false) == 0);
    for (const uint64_t frequency : {19'200'000ull, 24'000'000ull, 100'000'000ull, 1'000'000'000ull}) {
        const uint64_t start = frequency * 100;
        const uint64_t end = start + frequency / 100; // 10 ms host elapsed
        const auto old_shift = Scale(frequency, false);
        const uint64_t old_microseconds = ((end - start) << old_shift) * 1'000'000 / frequency;
        assert(old_microseconds == (10'000ull << old_shift));
        const auto fixed_shift = Scale(frequency, true);
        const uint64_t direct_tsc = (end - start) << fixed_shift;
        const uint64_t import_tsc = direct_tsc >> Scale(frequency, true);
        assert(direct_tsc == import_tsc);
        assert(direct_tsc * 1'000'000 / frequency == 10'000);
    }
    setenv("KYTY_DS_NATIVE_TSC", "1", 1);
    assert(Cpu::GuestTscPolicy::NativeCounterRequested());
    std::puts("TSC policy: direct/imported ticks and frequency agree; 19.2 MHz 64x mismatch reproduced");
}
