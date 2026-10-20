// KYTY_FTZ32 decision (graphics/shader/ftz32.h): an explicit denormal flush is dropped only for
// operations whose results under DenormFlushToZero match GCN's flush to a signed zero.
#include "graphics/shader/ftz32.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Libs::Graphics;

static void Check(bool c, const char* t) {
	if (!c) {
		std::fprintf(stderr, "Ftz32Tests: %s\n", t);
		std::abort();
	}
}

static std::vector<uint32_t> GcnResults() {
	std::vector<uint32_t> results(Ftz32::kInputs.size() * Ftz32::kStride, 0u);
	for (uint32_t i = 0; i < Ftz32::kInputs.size(); ++i)
		for (uint32_t op = 0; op < Ftz32::kOpCount; ++op)
			results[i * Ftz32::kStride + op] = Ftz32::GcnResult(op, Ftz32::kInputs[i]);
	return results;
}

int main() {
	const auto all = Ftz32::Rcp | Ftz32::Rsq | Ftz32::Sqrt | Ftz32::Exp2 | Ftz32::Log2;
	Check(Ftz32::OpsMatchingGcn(GcnResults()) == all, "a GCN-exact device keeps no flush");
	Check(Ftz32::GcnResult(0, 0x80000001u) == 0xff800000u, "rcp(-denorm) is -inf on GCN");
	Check(Ftz32::GcnResult(2, 0x807fffffu) == 0x80000000u, "sqrt(-denorm) is -0 on GCN");
	// A device whose flush drops the sign: rcp/rsq of negative denormals give +inf, sqrt +0.
	auto unsigned_zero = GcnResults();
	for (uint32_t i = 2; i < 4; ++i) {
		unsigned_zero[i * Ftz32::kStride + 0] = 0x7f800000u;
		unsigned_zero[i * Ftz32::kStride + 1] = 0x7f800000u;
		unsigned_zero[i * Ftz32::kStride + 2] = 0u;
	}
	Check(Ftz32::OpsMatchingGcn(unsigned_zero) == (Ftz32::Exp2 | Ftz32::Log2),
	      "a sign-dropping flush must keep the rcp/rsq/sqrt flushes");
	// A device that does not flush sqrt inputs: sqrt(-denorm) is NaN.
	auto no_flush = GcnResults();
	no_flush[2 * Ftz32::kStride + 2] = 0xffc00000u;
	Check((Ftz32::OpsMatchingGcn(no_flush) & Ftz32::Sqrt) == 0u, "an unflushed sqrt keeps its flush");
	Check(Ftz32::OpsMatchingGcn(std::vector<uint32_t>(4, 0u)) == 0u, "short result buffer drops nothing");
	// The probe module patch: capability + extension after the capabilities, the execution mode
	// right after the entry point.
	const std::vector<uint32_t> module {
	    0x07230203u, 0x00010000u, 0u, 10u, 0u,
	    (2u << 16u) | 17u, 1u,                 // OpCapability Shader
	    (3u << 16u) | 14u, 0u, 1u,             // OpMemoryModel Logical GLSL450
	    (5u << 16u) | 15u, 5u, 4u, 0x6e69616du, 0u, // OpEntryPoint GLCompute %4 "main"
	    (6u << 16u) | 16u, 4u, 17u, 1u, 1u, 1u, // OpExecutionMode %4 LocalSize 1 1 1
	};
	const auto patched = Ftz32::AddDenormFlushToZero32(module);
	Check(patched.size() == module.size() + 2u + 7u + 4u, "patch adds 13 words");
	Check(patched[5] == ((2u << 16u) | 17u) && patched[6] == 1u, "Shader capability first");
	Check(patched[7] == ((2u << 16u) | 17u) && patched[8] == 4465u, "DenormFlushToZero capability");
	Check(patched[9] == ((7u << 16u) | 10u), "SPV_KHR_float_controls extension");
	Check(patched[24] == ((4u << 16u) | 16u) && patched[25] == 4u && patched[26] == 4460u && patched[27] == 32u,
	      "execution mode after the entry point");
	Check(Ftz32::AddDenormFlushToZero32(std::vector<uint32_t>(module.begin(), module.begin() + 10)).empty(),
	      "a module without an entry point is rejected");
	std::puts("Ftz32Tests: ok");
	return 0;
}
