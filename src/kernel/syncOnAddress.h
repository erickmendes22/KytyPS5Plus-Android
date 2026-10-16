#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_

#include "common/abi.h"
#include "common/common.h"

#include <atomic>
#include <chrono>

namespace Libs::LibKernel::SyncOnAddress {

using signal_poll_func_t = void (*)();

// Where a guest thread is blocked inside Wait*, so a guest signal sent to it can end the wait at
// once instead of at the next signal-poll slice. Owned by the thread's Pthread record.
struct WaitSlot {
	std::atomic<volatile void*> address {nullptr};
};

// Publishes the calling thread's waits through `slot` (nullptr stops publishing).
void SetCurrentThreadWaitSlot(WaitSlot* slot);
// Best effort: wakes the wait the slot's thread is blocked in, if any. Waits may return early
// (a spurious wake-up), which wait-on-address callers already tolerate.
void WakeWaitSlot(WaitSlot* slot);

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, std::chrono::nanoseconds timeout,
           signal_poll_func_t signal_poll = nullptr);
int Wake(volatile void* address, int32_t count);

} // namespace Libs::LibKernel::SyncOnAddress

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_ */
