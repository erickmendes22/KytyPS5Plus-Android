#ifndef KYTY_CPU_GUEST_CPU_SELF_TEST_H_
#define KYTY_CPU_GUEST_CPU_SELF_TEST_H_

namespace Cpu::GuestCpu {

// Runs a short translator self-test on the calling thread: a guest add, an HLE round trip, a
// host-to-guest callback and a write fault through the host fault hook. Prints one
// "FEX-SELFTEST PASS|FAIL <case>" line per case to stdout and returns true when all cases pass.
// Intended for debug builds that need to prove FEXCore works inside the app process.
bool RunGuestCpuSelfTest();

} // namespace Cpu::GuestCpu

#endif /* KYTY_CPU_GUEST_CPU_SELF_TEST_H_ */
