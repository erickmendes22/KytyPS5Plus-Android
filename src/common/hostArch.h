#ifndef KYTY_COMMON_HOST_ARCH_H_
#define KYTY_COMMON_HOST_ARCH_H_

// Host CPU architecture. Guest PS5 code is always x86-64; on an ARM64 host it must run through a
// binary translator (FEXCore), so code that executes or inspects guest instructions natively is
// x86-host only.
#if defined(__x86_64__) || defined(_M_X64)
#define KYTY_HOST_X86   1
#define KYTY_HOST_ARM64 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#define KYTY_HOST_X86   0
#define KYTY_HOST_ARM64 1
#else
#error "Unsupported host architecture"
#endif

#if KYTY_HOST_ARM64
#include "common/assert.h"

// Marks host code that runs or patches guest x86 code directly. On ARM64 this work moves into the
// FEXCore integration (port Phase 2); until then reaching it is a hard stop with a clear message.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_ARM64_TODO(what) EXIT("ARM64 host: %s requires the guest CPU translator (not ported yet)\n", what)
#endif

#endif /* KYTY_COMMON_HOST_ARCH_H_ */
