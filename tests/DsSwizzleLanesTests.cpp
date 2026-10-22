// KYTY_SWIZZLE_RELATIVE: the relative-shuffle form of DS_SWIZZLE_B32 reads the same lanes as the
// generic indexed form (graphics/shader/recompiler/backend/spirv/dsSwizzleLanes.h).
#include "graphics/shader/recompiler/backend/spirv/dsSwizzleLanes.h"

#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics::ShaderRecompiler::Spirv;

static void Check(bool condition, const char* text, uint32_t control = 0, uint32_t lane = 0) {
	if (!condition) {
		std::fprintf(stderr, "DsSwizzleLanesTests: %s (control=0x%04x lane=%u)\n", text, control, lane);
		std::abort();
	}
}

int main() {
	// Rotate left by 1 / right by 1 (UE5 PPSA14708 cs b48e83a9: swizzle(ROTATE,0,1) / (ROTATE,1,1)).
	constexpr uint32_t rotate_left_1  = 0xc000u | (1u << 5u);
	constexpr uint32_t rotate_right_1 = 0xc000u | 0x400u | (1u << 5u);
	Check(DsSwizzle::SourceLane(rotate_left_1, 0) == 1, "rotate left reads the next lane");
	Check(DsSwizzle::SourceLane(rotate_left_1, 31) == 0, "rotate left wraps within the group");
	Check(DsSwizzle::SourceLane(rotate_left_1, 63) == 32, "rotate left wraps within the second group");
	Check(DsSwizzle::SourceLane(rotate_right_1, 0) == 31, "rotate right wraps to the group end");
	Check(DsSwizzle::Classify(rotate_left_1).kind == DsSwizzle::Relative::Kind::Rotate, "rotate classified");
	Check(DsSwizzle::Classify(0xc000u | 0x1u | (1u << 5u)).kind == DsSwizzle::Relative::Kind::None,
	      "a rotate that keeps lane bits stays generic");
	Check(DsSwizzle::Classify(0x1fu | (1u << 10u)).kind == DsSwizzle::Relative::Kind::Xor, "swap 1 is an xor");
	Check(DsSwizzle::Classify(0x1eu).kind == DsSwizzle::Relative::Kind::None, "a cleared lane bit stays generic");
	Check(DsSwizzle::Classify(0x8000u | 0xe4u).kind == DsSwizzle::Relative::Kind::None, "quad mode stays generic");

	// Exhaustive: every encodable control (FFT mode excluded) and every lane of a 128-lane subgroup.
	uint32_t relative_controls = 0;
	for (uint32_t control = 0; control < 0xe000u; control++) {
		const auto relative = DsSwizzle::Classify(control);
		for (uint32_t lane = 0; lane < 128u; lane++) {
			const auto source = DsSwizzle::SourceLane(control, lane);
			Check(source >> 5u == lane >> 5u, "the source stays in the 32-lane group", control, lane);
			if (relative.kind != DsSwizzle::Relative::Kind::None) {
				Check(DsSwizzle::RelativeSourceLane(relative, lane) == source,
				      "relative form reads the generic source lane", control, lane);
			}
		}
		relative_controls += relative.kind != DsSwizzle::Relative::Kind::None ? 1u : 0u;
	}
	// 2 rotate directions x 32 amounts x the ignored bits 0x2000/0x0800 + 32 xors x 8 ignored settings.
	Check(relative_controls > 64u, "rotates and xors are classified");
	std::printf("DsSwizzleLanesTests: ok (%u relative controls)\n", relative_controls);
	return 0;
}
