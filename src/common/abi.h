#ifndef KYTY_COMMON_ABI_H_
#define KYTY_COMMON_ABI_H_

#include "common/common.h"
#include "common/hostArch.h"

#if KYTY_HOST_X86
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_MS_ABI __attribute__((ms_abi))

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_SYSV_ABI __attribute__((sysv_abi))
#else
// On ARM64 hosts guest code never calls host functions directly: the guest CPU translator marshals
// x86-64 SysV arguments into native AAPCS64 calls, so these calling-convention attributes are no-ops.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_MS_ABI

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_SYSV_ABI
#endif

#endif /* KYTY_COMMON_ABI_H_ */
