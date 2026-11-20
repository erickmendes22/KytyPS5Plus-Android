#include "kernel/threadPlacement.h"
#include <cassert>
#include <cstdint>

using Kernel::ThreadPlacement::PrimeMask;
using Kernel::ThreadPlacement::kModeBits;
using Kernel::ThreadPlacement::kPreferPrime;
static_assert(kPreferPrime == 16);
static_assert(kModeBits >= 17);
// A fast policy-limited cluster does not supplant the silicon's prime cores.
// Hardware maxima select CPU 7 on a heterogeneous 8-core example.
constexpr long max_frequency[8] = {3033000, 3033000, 3033000, 3033000,
                                   3500000, 3500000, 3500000, 4100000};
static_assert(PrimeMask(max_frequency, 8) == (uint64_t{1} << 7));
constexpr long uniform_frequency[4] = {3000000, 3000000, 3000000, 3000000};
static_assert(PrimeMask(uniform_frequency, 4) == 0);
// The UFC6 mode has no exclusive (bit 4) or split (bit 8) placement.
static_assert((17 & (4 | 8)) == 0);
int main() {
    assert(PrimeMask(max_frequency, 8) == (uint64_t{1} << 7));
}
