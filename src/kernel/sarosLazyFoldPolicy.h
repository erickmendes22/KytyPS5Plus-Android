#pragma once
#include "kernel/addressFold.h"
#include <cstdint>
namespace Kernel::SarosLazyFoldPolicy {
constexpr bool NeedActivation(bool saros, bool already_enabled,
                              uint64_t start, uint64_t length) {
    if (!saros || already_enabled || start >= AddressFold::kFoldSize ||
        start >= AddressFold::kDomainEnd || !length) return false;
    return length > AddressFold::kFoldSize - start;
}
} // namespace Kernel::SarosLazyFoldPolicy
