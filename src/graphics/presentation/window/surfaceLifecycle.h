#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_SURFACELIFECYCLE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_SURFACELIFECYCLE_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

// Decides, before every present, whether the window surface may be used. Android takes the
// ANativeWindow away when the app goes to the background; presenting into it then fails with
// VK_ERROR_SURFACE_LOST_KHR and no new surface can be made until the app returns.
//
// EnterBackground/EnterForeground run on the thread that raises the lifecycle event (an SDL event
// watch, before SDL releases the window). Everything else runs on the presentation thread.
class SurfaceLifecycle final {
public:
	enum class Action : uint8_t {
		Present,  // the swapchain is current: acquire and present
		Teardown, // the window is going or gone: destroy swapchain and surface, then ask again
		Rebuild,  // in the foreground without a surface: create surface and swapchain
		Drop,     // no usable surface: complete the flip without presenting
	};

	// Wait before trying again after a rebuild failed or the surface was lost while presenting.
	static constexpr uint64_t RetryDelayMs = 250;

	void EnterBackground() noexcept {
		m_generation.fetch_add(1, std::memory_order_acq_rel);
		m_background.store(true, std::memory_order_release);
	}
	void EnterForeground() noexcept { m_background.store(false, std::memory_order_release); }
	[[nodiscard]] bool InBackground() const noexcept {
		return m_background.load(std::memory_order_acquire);
	}

	[[nodiscard]] Action Next(uint64_t now_ms) noexcept {
		const bool     background = m_background.load(std::memory_order_acquire);
		const uint64_t generation = m_generation.load(std::memory_order_acquire);
		if (m_has_surface) {
			// A surface made before the last background refers to the window Android took away.
			return background || generation != m_surface_generation ? Action::Teardown
			                                                        : Action::Present;
		}
		if (background || now_ms < m_retry_at_ms) {
			return Action::Drop;
		}
		m_pending_generation = generation;
		return Action::Rebuild;
	}

	void TornDown() noexcept {
		m_has_surface = false;
		m_retry_at_ms = 0;
	}

	// Result of the Rebuild that Next asked for.
	void Rebuilt(bool ok, uint64_t now_ms) noexcept {
		m_has_surface = ok;
		if (ok) {
			m_surface_generation = m_pending_generation;
			m_failed_rebuilds    = 0;
		} else {
			m_retry_at_ms = now_ms + RetryDelayMs;
			m_failed_rebuilds++;
		}
	}

	// The surface was lost while presenting and could not be recreated at once.
	void Lost(uint64_t now_ms) noexcept {
		m_has_surface = false;
		m_retry_at_ms = now_ms + RetryDelayMs;
	}

	[[nodiscard]] bool     HasSurface() const noexcept { return m_has_surface; }
	[[nodiscard]] uint32_t FailedRebuilds() const noexcept { return m_failed_rebuilds; }

private:
	std::atomic_bool      m_background {false};
	std::atomic<uint64_t> m_generation {0};
	// Presentation thread only. The surface made at startup belongs to generation 0.
	bool     m_has_surface        = true;
	uint64_t m_surface_generation = 0;
	uint64_t m_pending_generation = 0;
	uint64_t m_retry_at_ms        = 0;
	uint32_t m_failed_rebuilds    = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_SURFACELIFECYCLE_H_
