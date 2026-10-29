#ifndef KYTY_PIPELINE_ALLOCATION_RECOVERY_H_
#define KYTY_PIPELINE_ALLOCATION_RECOVERY_H_

#include <cstdint>

namespace Libs::Graphics {

// VkResult values: OUT_OF_HOST_MEMORY=-1, OUT_OF_DEVICE_MEMORY=-2. Do not
// retry device loss, invalid shaders, or any other driver error as memory pressure.
constexpr bool IsPipelineMemoryFailure(int32_t result) {
	return result == -1 || result == -2;
}

// The worker only reports the failure. All callbacks execute on the recording
// thread, in this order. In particular no GPU-owned handle is retired before
// the scheduler drains, and a repeated OOM never starts an unbounded retry loop.
template <typename ReleaseFailed, typename Drain, typename Reclaim, typename Retry>
int32_t RecoverPipelineAllocation(int32_t result, ReleaseFailed&& release_failed,
                                 Drain&& drain, Reclaim&& reclaim, Retry&& retry) {
	if (!IsPipelineMemoryFailure(result)) return result;
	release_failed();
	drain();
	reclaim();
	return retry();
}

} // namespace Libs::Graphics

#endif
