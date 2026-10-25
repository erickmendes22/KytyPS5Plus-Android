#ifndef KYTY_LOADER_MFGT_INDEX_HEADER_H_
#define KYTY_LOADER_MFGT_INDEX_HEADER_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace Loader::MfgtIndexHeader {
inline constexpr size_t kPrefixSize = 72;
struct Result {
	bool               complete = false;
	uint32_t           magic    = 0;
	uint32_t           version  = 0;
	[[nodiscard]] bool Compatible() const {
		return complete && magic == 0x122bb8f2 && version == 0x11160000;
	}
};
// Exact transform from the verified PPSA24156 executable, RVA 0x3d4ba70.
// This reads a separate 72-byte prefix, never changes the game's file or buffers.
[[nodiscard]] inline Result Decode(std::span<const uint8_t> raw) {
	if (raw.size() < kPrefixSize) return {};
	uint32_t seed = 0x1fd5b434;
	Result   result {true};
	for (size_t offset = 0; offset < kPrefixSize; offset += 4) {
		uint32_t cipher = 0;
		for (unsigned j = 0; j < 4; ++j)
			cipher |= uint32_t(raw[offset + j]) << (8 * j);
		uint32_t crc = seed;
		for (unsigned bit = 0; bit < 32; ++bit)
			crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
		const uint32_t decoded = ~(crc ^ cipher);
		if (offset == 0) result.magic = decoded;
		if (offset == 0x20) result.version = decoded;
		seed = cipher;
	}
	return result;
}
} // namespace Loader::MfgtIndexHeader
#endif
