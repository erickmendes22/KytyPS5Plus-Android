#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_MESHEMULATIONRING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_MESHEMULATIONRING_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;

// GPU-only ring for mesh emulation: the compute pass of each emulated mesh draw writes its vertex
// records and triangle indices here and the draw reads them back. A range is free again once the
// command buffer (tick) that used it has completed. Wrapping onto a range the current command
// buffer still uses cannot wait for it; the reservation reports that instead and the caller
// orders the reuse with a pipeline barrier. A request larger than the ring grows it; the old
// buffer is kept until its last tick completes.
class MeshEmulationRing {
public:
	struct Reservation {
		vk::Buffer        buffer  = nullptr;
		uint64_t          offset  = 0;
		vk::DeviceAddress address = 0;
		// The range was used earlier in the current command buffer.
		bool reuses_current = false;
	};

	static constexpr uint64_t InitialSize = 64ull * 1024 * 1024;
	static constexpr uint64_t Alignment   = 256;

	MeshEmulationRing(GraphicContext& graphics, CommandScheduler& scheduler,
	                  uint64_t size = InitialSize, const char* name = "Kyty.MeshEmulationRing");
	~MeshEmulationRing();
	KYTY_CLASS_NO_COPY(MeshEmulationRing);

	// KYTY_MESH_RING_MIB, else InitialSize.
	[[nodiscard]] static uint64_t ConfiguredInitialSize();
	// KYTY_MESH_RING_GROW_MIB (diagnostic), else 0.
	[[nodiscard]] static uint64_t ConfiguredForcedGrowth();

	[[nodiscard]] Reservation Reserve(uint64_t size);
	[[nodiscard]] uint64_t    Capacity() const noexcept { return m_buffer->Size(); }
	// Device-loss diagnostics: logs the current and retired buffers holding any address.
	void DescribeFaultAddresses(std::span<const uint64_t> addresses) const;
	// Device address of the counter the vertex-pull shader increments for each vertex index past
	// its draw's records (meshVertexPullShader.cpp); a host-visible word, created on first use.
	[[nodiscard]] vk::DeviceAddress OutOfRangeCounterAddress();
	// Logs the counter when it changed (at most once a second; also from DescribeFaultAddresses).
	void PollOutOfRangeCounter(bool force = false);
	// KYTY_MESH_BOUNDS=1 (graphics/shader/meshBoundsCheck.h): allocates the violation log of the
	// emulated mesh compute stage and publishes its address to the shader emitter. Call before
	// mesh shaders compile.
	void EnableBoundsLog();
	// Logs violations recorded since the last poll (at most once a second unless forced). The
	// const form logs every recorded entry (GPU fatal hook).
	void PollBoundsLog(bool force = false);
	void DumpBoundsLog() const;

private:
	struct Range {
		uint64_t tick  = 0;
		uint64_t begin = 0;
		uint64_t end   = 0;
	};

	void               Grow(uint64_t size, const char* reason);
	void               ReleaseRetired();
	[[nodiscard]] bool KnownFree(uint64_t tick) const;

	GraphicContext&                                       m_graphics;
	CommandScheduler&                                     m_scheduler;
	const char*                                           m_name;
	std::unique_ptr<Buffer>                               m_buffer;
	uint64_t                                              m_offset = 0;
	uint64_t m_forced_growth = ConfiguredForcedGrowth();
	std::unique_ptr<Buffer>                               m_out_of_range;
	uint32_t                                              m_out_of_range_logged = 0;
	int64_t                                               m_out_of_range_polled_ms = 0;
	std::unique_ptr<Buffer>                               m_bounds_log;
	uint32_t                                              m_bounds_logged = 0;
	int64_t                                               m_bounds_polled_ms = 0;
	std::deque<Range>                                     m_ranges;
	std::vector<std::pair<std::unique_ptr<Buffer>, uint64_t>> m_retired;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_MESHEMULATIONRING_H_
