#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_REFRESHCAUSE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_REFRESHCAUSE_H_

#include <pthread.h>

#include <array>
#include <atomic>
#include <cstdint>

// --perf-stats diagnostics: what first invalidated an image's guest bytes, so the [img-refresh]
// lines can say why an image is uploaded again (UE5 PPSA14708 uploads ~0.8 GB of render
// targets per frame). Purely descriptive: nothing here changes what is invalidated or uploaded.
namespace Libs::Graphics::RefreshCause {

enum class Source : uint8_t {
	New,         // a newly created image (starts CPU-dirty)
	WriteFault,  // a guest CPU write fault
	Kernel,      // RenderContext::InvalidateMemory (file reads, kernel copies)
	Fill,        // BufferCache::FillMemory's CPU path
	Copy,        // BufferCache::CopyBuffer's CPU path
	GpuBuffer,   // a GPU write through a buffer (DMA fill/copy, shader storage write)
	Other,
	Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Source::Count)> kNames {
    "new", "fault", "kernel", "fill", "copy", "gpu-buffer", "other"};

[[nodiscard]] inline const char* Name(Source source) {
	const auto index = static_cast<size_t>(source);
	return index < kNames.size() ? kNames[index] : "?";
}

namespace Detail {
// No thread_local: the fault path runs in a signal handler of a dlopen'ed library.
inline std::atomic<pthread_t> g_writer {};
inline std::atomic<bool>      g_active {false};
inline std::atomic<uint8_t>   g_source {static_cast<uint8_t>(Source::Other)};
} // namespace Detail

// While alive, write faults this thread takes are attributed to `source` instead of WriteFault.
// One scope at a time (the GPU thread's fill and copy fast paths); a second thread's scope
// simply loses attribution, which only affects the diagnostic.
class CpuWriteScope {
public:
	explicit CpuWriteScope(Source source) {
		bool expected = false;
		if (Detail::g_active.compare_exchange_strong(expected, true)) {
			Detail::g_writer.store(pthread_self());
			Detail::g_source.store(static_cast<uint8_t>(source));
			m_owner = true;
		}
	}
	~CpuWriteScope() {
		if (m_owner) {
			Detail::g_active.store(false);
		}
	}
	CpuWriteScope(const CpuWriteScope&)            = delete;
	CpuWriteScope& operator=(const CpuWriteScope&) = delete;

private:
	bool m_owner = false;
};

// The source of a write fault taken on the calling thread.
[[nodiscard]] inline Source ForFault() {
	if (Detail::g_active.load() && pthread_equal(Detail::g_writer.load(), pthread_self()) != 0) {
		return static_cast<Source>(Detail::g_source.load());
	}
	return Source::WriteFault;
}

} // namespace Libs::Graphics::RefreshCause

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_REFRESHCAUSE_H_ */
