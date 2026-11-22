#pragma once

#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics::DemonsRadianceTrace {

inline constexpr uint64_t Address = 0x21dd00000ull;
inline constexpr uint64_t ByteSize = 256ull * 256 * 8;
inline constexpr uint64_t MetadataAddress = 0x20907a000ull;
inline constexpr uint64_t MetadataSize = 0x1000;

constexpr bool Overlap(uint64_t base, uint64_t size, uint64_t target, uint64_t bytes) {
	return size != 0 && bytes != 0 &&
	       (base >= target ? base - target < bytes : target - base < size);
}

constexpr bool Data(uint64_t address, uint64_t size) {
	return Overlap(address, size, Address, ByteSize);
}

constexpr bool Metadata(uint64_t address, uint64_t size) {
	return Overlap(address, size, MetadataAddress, MetadataSize);
}

inline bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_DS_FRAME_TRACE");
		return value && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

} // namespace Libs::Graphics::DemonsRadianceTrace
