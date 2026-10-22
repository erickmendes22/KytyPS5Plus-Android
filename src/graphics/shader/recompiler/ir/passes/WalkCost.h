#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_WALKCOST_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_WALKCOST_H_

#include <atomic>
#include <chrono>
#include <cstdint>

// Running totals of SRT walk time (MaterializeResources) on every thread that walks: the front,
// fan-out workers and the record thread. Off unless KYTY_GPU_FAN=auto turns it on (walkFanConfig.cpp,
// FanAutoPolicy); then each walk costs two steady_clock reads and two relaxed atomic adds.
namespace Libs::Graphics::ShaderRecompiler::IR::WalkCost {

inline std::atomic<bool>     g_enabled {false};
inline std::atomic<uint64_t> g_walk_ns {0};
inline std::atomic<uint64_t> g_walks {0};

class Scope {
public:
	Scope() noexcept: m_on(g_enabled.load(std::memory_order_relaxed)) {
		if (m_on) {
			m_start = std::chrono::steady_clock::now();
		}
	}
	~Scope() {
		if (m_on) {
			const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			    std::chrono::steady_clock::now() - m_start);
			g_walk_ns.fetch_add(static_cast<uint64_t>(ns.count()), std::memory_order_relaxed);
			g_walks.fetch_add(1, std::memory_order_relaxed);
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	bool                                  m_on;
	std::chrono::steady_clock::time_point m_start {};
};

} // namespace Libs::Graphics::ShaderRecompiler::IR::WalkCost

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_WALKCOST_H_
