#ifndef KYTY_LIBS_APR_READ_RANGE_H_
#define KYTY_LIBS_APR_READ_RANGE_H_

#include "kernel/addressFold.h"

namespace Libs::AprReadRange {
// Validate the backing span, retaining the original guest address in queued records.
// The fold policy is explicit so ARM64 limits are also testable on x86 CI hosts.
[[nodiscard]] constexpr bool Resolve(uint64_t guest, uint64_t size, uint64_t limit,
                                     bool fold, uint64_t* host) {
	if (size == 0 || size > 0x100000000ull) return false;
	uint64_t backing = guest;
	if (fold && !Kernel::AddressFold::FoldSpan(guest, size, &backing)) return false;
	if (backing > limit || size > limit - backing) return false;
	*host = backing;
	return true;
}
} // namespace Libs::AprReadRange
#endif
