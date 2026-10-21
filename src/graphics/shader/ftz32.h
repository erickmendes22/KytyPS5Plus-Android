#ifndef EMULATOR_SRC_GRAPHICS_SHADER_FTZ32_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_FTZ32_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

// KYTY_FTZ32: GCN flushes FP32 denormal inputs of rcp/rsq/sqrt/exp2/log2 to a zero of the same
// sign, which the shader recompiler emits as an explicit flush in front of each such operation
// (4 ALU ops). Under the SPIR-V DenormFlushToZero execution mode (32-bit) a device flushes on its
// own, but Vulkan does not promise to keep the sign. A device check (host_gpu/.../ftzProbe.cpp)
// runs each operation on denormal inputs under that mode; an operation keeps its explicit flush
// unless every result equals the GCN one.
namespace Libs::Graphics::Ftz32 {

enum Op : uint32_t {
	Rcp  = 1u << 0u,
	Rsq  = 1u << 1u,
	Sqrt = 1u << 2u,
	Exp2 = 1u << 3u,
	Log2 = 1u << 4u,
};
inline constexpr uint32_t kOpCount = 5;

// Denormal inputs of the check: smallest and largest magnitude, both signs.
inline constexpr std::array<uint32_t, 4> kInputs {0x00000001u, 0x007fffffu, 0x80000001u, 0x807fffffu};
// Results the check writes: results[input * kStride + op index].
inline constexpr uint32_t kStride = 8;

// Whether the device check runs: KYTY_FTZ32 unset or empty follows `default_on` (Mesa Turnip),
// "0" turns it off, anything else on.
[[nodiscard]] constexpr bool Enabled(const char* env, bool default_on) {
	if (env == nullptr || env[0] == '\0') return default_on;
	return !(env[0] == '0' && env[1] == '\0');
}

// The GCN result of operation `op_index` (bit order of Op) for denormal input `input`.
[[nodiscard]] constexpr uint32_t GcnResult(uint32_t op_index, uint32_t input) {
	const uint32_t sign = input & 0x80000000u;
	switch (op_index) {
		case 0: // rcp(+-0) = +-inf
		case 1: return sign | 0x7f800000u; // rsq(+-0) = +-inf
		case 2: return sign;               // sqrt(+-0) = +-0
		case 3: return 0x3f800000u;        // exp2(0) = 1
		default: return 0xff800000u;       // log2(+-0) = -inf
	}
}

// The operations whose device results match GCN for every input (Op bits).
[[nodiscard]] constexpr uint32_t OpsMatchingGcn(std::span<const uint32_t> results) {
	uint32_t ops = 0;
	if (results.size() < kInputs.size() * kStride) return 0;
	for (uint32_t op = 0; op < kOpCount; ++op) {
		bool match = true;
		for (uint32_t i = 0; i < kInputs.size(); ++i) {
			match &= results[i * kStride + op] == GcnResult(op, kInputs[i]);
		}
		if (match) ops |= 1u << op;
	}
	return ops;
}


namespace Detail {

inline constexpr uint32_t kOpExtension           = 10;
inline constexpr uint32_t kOpEntryPoint          = 15;
inline constexpr uint32_t kOpExecutionMode       = 16;
inline constexpr uint32_t kOpCapability          = 17;
inline constexpr uint32_t kCapDenormFlushToZero  = 4465;
inline constexpr uint32_t kModeDenormFlushToZero = 4460;

inline uint32_t Word(uint32_t count, uint32_t opcode) {
	return (count << 16u) | opcode;
}

inline void AppendString(std::vector<uint32_t>& words, std::string_view text) {
	const size_t count = text.size() / 4u + 1u; // includes the terminating zero
	const size_t first = words.size();
	words.resize(first + count, 0u);
	std::memcpy(words.data() + first, text.data(), text.size());
}

} // namespace Detail

// `module` with OpCapability DenormFlushToZero, OpExtension "SPV_KHR_float_controls" and
// OpExecutionMode <entry> DenormFlushToZero 32 added (the device check's GLSL module: glslang has
// no syntax for them). Empty when the module has no entry point or is malformed.
inline std::vector<uint32_t> AddDenormFlushToZero32(std::span<const uint32_t> module) {
	constexpr size_t kHeader = 5;
	if (module.size() <= kHeader) return {};
	std::vector<uint32_t> out(module.begin(), module.begin() + kHeader);
	bool     capability_added = false;
	bool     mode_added       = false;
	uint32_t entry            = 0;
	size_t   pos              = kHeader;
	while (pos < module.size()) {
		const uint32_t count  = module[pos] >> 16u;
		const uint32_t opcode = module[pos] & 0xffffu;
		if (count == 0 || pos + count > module.size()) return {};
		// The capability and the extension go in front of the first instruction past the
		// capabilities; the execution mode right after the entry points.
		if (!capability_added && opcode != Detail::kOpCapability) {
			out.push_back(Detail::Word(2, Detail::kOpCapability));
			out.push_back(Detail::kCapDenormFlushToZero);
			std::vector<uint32_t> name;
			Detail::AppendString(name, "SPV_KHR_float_controls");
			out.push_back(Detail::Word(static_cast<uint32_t>(1 + name.size()), Detail::kOpExtension));
			out.insert(out.end(), name.begin(), name.end());
			capability_added = true;
		}
		if (opcode == Detail::kOpEntryPoint && count >= 3) entry = module[pos + 2];
		if (!mode_added && entry != 0 && opcode != Detail::kOpEntryPoint) {
			out.push_back(Detail::Word(4, Detail::kOpExecutionMode));
			out.push_back(entry);
			out.push_back(Detail::kModeDenormFlushToZero);
			out.push_back(32u);
			mode_added = true;
		}
		out.insert(out.end(), module.begin() + pos, module.begin() + pos + count);
		pos += count;
	}
	return mode_added ? out : std::vector<uint32_t> {};
}


} // namespace Libs::Graphics::Ftz32

#endif // EMULATOR_SRC_GRAPHICS_SHADER_FTZ32_H_
