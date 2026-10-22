#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_DSSWIZZLELANES_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_DSSWIZZLELANES_H_

#include <cstdint>

// DS_SWIZZLE_B32 source lanes (RDNA2 ISA, "DS_SWIZZLE_B32"), and the subset that a relative
// shuffle by a constant reads (KYTY_SWIZZLE_RELATIVE, spirvEmitterMemory.cpp). Pure functions,
// shared by the SPIR-V emitter and tests/DsSwizzleLanesTests.cpp.
namespace Libs::Graphics::ShaderRecompiler::Spirv::DsSwizzle {

// The lane `lane` (host subgroup invocation) reads; swizzles stay within its 32-lane group.
// Same arithmetic as EmitDsSwizzleTargetLane (FFT mode, offset >= 0xe000, is rejected at decode).
[[nodiscard]] constexpr uint32_t SourceLane(uint32_t control, uint32_t lane) {
	const uint32_t base  = lane & ~31u;
	const uint32_t local = lane & 31u;
	if ((control & 0xc000u) == 0xc000u) {
		const uint32_t mask   = control & 0x1fu;
		const uint32_t rotate = (control >> 5u) & 0x1fu;
		const uint32_t delta  = (control & 0x400u) != 0u ? ((32u - rotate) & 0x1fu) : rotate;
		const uint32_t rotated = (local + delta) & 31u;
		return base | (local & mask) | (rotated & (~mask & 31u));
	}
	if ((control & 0x8000u) != 0u) {
		const uint32_t position = (control >> ((local & 3u) * 2u)) & 3u;
		return (lane & ~3u) | position;
	}
	const uint32_t and_mask = control & 0x1fu;
	const uint32_t or_mask  = (control >> 5u) & 0x1fu;
	const uint32_t xor_mask = (control >> 10u) & 0x1fu;
	return base | (((local & and_mask) | or_mask) ^ xor_mask);
}

struct Relative {
	enum class Kind : uint8_t { None, Rotate, Xor };
	Kind     kind   = Kind::None;
	uint32_t amount = 0; // Rotate: lane (l + amount) & 31 of the group; Xor: lane l ^ amount
};

// Swizzles one relative shuffle (plus a wrap select for rotates) reproduces: rotate mode with no
// kept lane bits, and bit-mask mode that keeps every lane bit with no OR (a pure XOR).
[[nodiscard]] constexpr Relative Classify(uint32_t control) {
	if ((control & 0xc000u) == 0xc000u) {
		if ((control & 0x1fu) != 0u) {
			return {};
		}
		const uint32_t rotate = (control >> 5u) & 0x1fu;
		return {Relative::Kind::Rotate, (control & 0x400u) != 0u ? ((32u - rotate) & 0x1fu) : rotate};
	}
	if ((control & 0x8000u) == 0u && (control & 0x1fu) == 0x1fu && ((control >> 5u) & 0x1fu) == 0u) {
		return {Relative::Kind::Xor, (control >> 10u) & 0x1fu};
	}
	return {};
}

// Whether a rotate reads from the start of the group (ShuffleUp by 32 - amount) rather than
// `amount` lanes further (ShuffleDown by amount).
[[nodiscard]] constexpr bool RotateWraps(uint32_t amount, uint32_t lane) {
	return amount != 0u && (lane & 31u) >= 32u - amount;
}

// The lane the relative form reads: ShuffleDown/ShuffleUp/ShuffleXor arithmetic.
[[nodiscard]] constexpr uint32_t RelativeSourceLane(const Relative& relative, uint32_t lane) {
	if (relative.amount == 0u) {
		return lane;
	}
	if (relative.kind == Relative::Kind::Xor) {
		return lane ^ relative.amount;
	}
	return RotateWraps(relative.amount, lane) ? lane - (32u - relative.amount) : lane + relative.amount;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::DsSwizzle

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_DSSWIZZLELANES_H_ */
