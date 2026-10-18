#ifndef GRAPHICS_SHADER_MESH_BOUNDS_CHECK_H
#define GRAPHICS_SHADER_MESH_BOUNDS_CHECK_H

// KYTY_MESH_BOUNDS=1 (diagnostic, off by default): the compute stage of emulated mesh shaders
// checks what Khronos GPU-assisted validation would (it hangs Astro Bot on Turnip) for the
// accesses it makes through buffer device addresses:
// - every vertex-record and triangle-index store against the dwords its dispatch reserved in the
//   mesh emulation ring (workgroup slots = groups along x * gl_NumWorkGroups.y);
// - every guest-memory load's page against the BDA page table's length.
// A violating access is skipped and recorded in a host-visible log buffer, read back by the
// renderer (MeshEmulationRing::PollBoundsLog): dword 0 counts violations, entries of
// kEntryDwords follow (guest shader hash low/high, kind, value, limit, workgroup x).
// The vertex-pull stage always clamps its vertex index and counts violations
// (MeshEmulationRing::OutOfRangeCounterAddress).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::MeshBoundsCheck {

enum class Kind : uint32_t {
	VertexStore = 1, // value: record dword index, limit: reserved record dwords
	IndexStore  = 2, // value: index dword index, limit: reserved index dwords
	BdaPage     = 3, // value: page index (low 32 bits), limit: page table entries (low 32 bits)
};

inline constexpr uint32_t kHeaderDwords = 4;
inline constexpr uint32_t kEntryDwords  = 6;
inline constexpr uint32_t kMaxEntries   = 64;
inline constexpr uint32_t kLogBytes     = (kHeaderDwords + kEntryDwords * kMaxEntries) * 4u;

[[nodiscard]] inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_MESH_BOUNDS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

namespace Detail {
inline std::atomic<uint64_t> g_log_address {0};
} // namespace Detail

// Set once by the renderer before mesh shaders compile; 0: checks skip the access but record
// nothing.
inline void SetLogAddress(uint64_t address) {
	Detail::g_log_address.store(address, std::memory_order_release);
}
[[nodiscard]] inline uint64_t LogAddress() {
	return Detail::g_log_address.load(std::memory_order_acquire);
}

[[nodiscard]] inline const char* KindName(uint32_t kind) {
	switch (static_cast<Kind>(kind)) {
		case Kind::VertexStore: return "vertex-store";
		case Kind::IndexStore: return "index-store";
		case Kind::BdaPage: return "bda-page";
	}
	return "?";
}

} // namespace Libs::Graphics::MeshBoundsCheck

#endif // GRAPHICS_SHADER_MESH_BOUNDS_CHECK_H
