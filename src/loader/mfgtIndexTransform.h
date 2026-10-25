#ifndef KYTY_LOADER_MFGT_INDEX_TRANSFORM_H_
#define KYTY_LOADER_MFGT_INDEX_TRANSFORM_H_

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Loader::MfgtIndexTransform {
using State = std::array<uint32_t, 16>;
inline uint32_t Read32(std::span<const uint8_t> b, size_t offset) {
	uint32_t v = 0;
	for (unsigned i = 0; i < 4; ++i)
		v |= uint32_t(b[offset + i]) << (8 * i);
	return v;
}
inline void Write32(std::span<uint8_t> b, size_t offset, uint32_t v) {
	for (unsigned i = 0; i < 4; ++i)
		b[offset + i] = static_cast<uint8_t>(v >> (8 * i));
}
// Exact scalar equivalent of the verified game's 20-round vector stream transform.
[[nodiscard]] inline State Block(const State& initial) {
	State      x  = initial;
	const auto qr = [&](size_t a, size_t b, size_t c, size_t d) {
		x[a] += x[b];
		x[d] = std::rotl(x[d] ^ x[a], 16);
		x[c] += x[d];
		x[b] = std::rotl(x[b] ^ x[c], 12);
		x[a] += x[b];
		x[d] = std::rotl(x[d] ^ x[a], 8);
		x[c] += x[d];
		x[b] = std::rotl(x[b] ^ x[c], 7);
	};
	for (unsigned i = 0; i < 10; ++i) {
		qr(0, 4, 8, 12);
		qr(1, 5, 9, 13);
		qr(2, 6, 10, 14);
		qr(3, 7, 11, 15);
		qr(0, 5, 10, 15);
		qr(1, 6, 11, 12);
		qr(2, 7, 8, 13);
		qr(3, 4, 9, 14);
	}
	for (size_t i = 0; i < x.size(); ++i)
		x[i] += initial[i];
	return x;
}
inline void Transform(State& state, std::span<uint8_t> data, uint64_t offset) {
	state[12]     = static_cast<uint32_t>(offset >> 6);
	size_t within = offset & 63;
	size_t cursor = 0;
	while (cursor < data.size()) {
		const auto block = Block(state);
		while (within < 64 && cursor < data.size()) {
			data[cursor++] ^= static_cast<uint8_t>(block[within / 4] >> ((within % 4) * 8));
			++within;
		}
		if (within == 64) {
			++state[12];
			within = 0;
		}
	}
}
[[nodiscard]] inline bool TransformHeader(std::span<uint8_t> data) {
	if (data.size() < 0x800) return false;
	uint32_t seed = 0x1fd5b434;
	for (size_t offset = 0; offset < 0x800; offset += 4) {
		const auto cipher = Read32(data, offset);
		uint32_t   crc    = seed;
		for (unsigned bit = 0; bit < 32; ++bit)
			crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
		Write32(data, offset, ~(crc ^ cipher));
		seed = cipher;
	}
	return true;
}
// CALL -> nearby MOVABS/JMP trampoline: preserve the guest return address and ABI.
[[nodiscard]] inline bool Call(uint64_t pc, uint64_t target, std::array<uint8_t, 5>& out) {
	if (pc > UINT64_MAX - 5) return false;
	const uint64_t end          = pc + 5;
	uint32_t       displacement = 0;
	if (target >= end) {
		if (target - end > uint64_t(INT32_MAX)) return false;
		displacement = static_cast<uint32_t>(target - end);
	} else {
		if (end - target > uint64_t(INT32_MAX) + 1) return false;
		displacement = uint32_t {0} - static_cast<uint32_t>(end - target);
	}
	out[0] = 0xe8;
	for (unsigned i = 0; i < 4; ++i)
		out[i + 1] = static_cast<uint8_t>(displacement >> (8 * i));
	return true;
}
[[nodiscard]] inline std::array<uint8_t, 12> Jump(uint64_t target) {
	std::array<uint8_t, 12> out {0x48, 0xb8};
	for (unsigned i = 0; i < 8; ++i)
		out[i + 2] = static_cast<uint8_t>(target >> (8 * i));
	out[10] = 0xff;
	out[11] = 0xe0;
	return out;
}
inline constexpr uint64_t               kStateVtable    = 0x5555b70;
inline constexpr uint64_t               kReferenceState = 0x4afa250;
inline constexpr size_t                 kStreamCall     = 0x3d4ef3a;
inline constexpr size_t                 kHeaderCall     = 0x3d4ebb8;
inline constexpr std::array<uint8_t, 5> kStreamOriginal {0xe8, 0x81, 0xb4, 0x09, 0x00};
inline constexpr std::array<uint8_t, 5> kHeaderOriginal {0xe8, 0xb3, 0xce, 0xff, 0xff};
} // namespace Loader::MfgtIndexTransform
#endif
