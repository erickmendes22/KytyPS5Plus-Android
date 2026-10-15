#include "libs/vaThunk.h"
#include "loader/guestAbiInvoke.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace GuestAbiTestFns {

// Digits(a, b, ...) = decimal concatenation of the arguments. Covers every integer arity.
template <typename... Ts>
int64_t Digits(Ts... xs) {
	int64_t result = 0;
	((result = result * 10 + static_cast<int64_t>(xs)), ...);
	return result;
}

double Sum9d(double a0, double a1, double a2, double a3, double a4, double a5, double a6,
             double a7, double a8) {
	return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8;
}

// Weights each argument by its position so that any reordering changes the result.
double Mixed(int32_t a, double b, uint8_t c, float d, int64_t e) {
	return a * 1.0 + b * 2.0 + c * 4.0 + d * 8.0 + e * 16.0;
}

// i0..i5 in RDI..R9; d0..d7 in XMM0..7; s0 is stack slot 0, d8 slot 1, s1 slot 2.
int64_t Spill17(int64_t i0, [[maybe_unused]] int64_t i1, [[maybe_unused]] int64_t i2,
                [[maybe_unused]] int64_t i3, [[maybe_unused]] int64_t i4, int64_t i5, double d0,
                [[maybe_unused]] double d1, [[maybe_unused]] double d2,
                [[maybe_unused]] double d3, [[maybe_unused]] double d4,
                [[maybe_unused]] double d5, [[maybe_unused]] double d6, double d7, int64_t s0,
                double d8, int64_t s1) {
	return i0 + i5 * 10 + static_cast<int64_t>(d0) * 100 + static_cast<int64_t>(d7) * 1000 +
	       s0 * 10000 + static_cast<int64_t>(d8) * 100000 + s1 * 1000000;
}

int32_t* PickPtr(const char* s, int32_t* out) {
	*out = s[0];
	return out + 1;
}

bool Flip(bool b) {
	return !b;
}

uint8_t Inc8(uint8_t x) {
	return static_cast<uint8_t>(x + 1);
}

int8_t Id8(int8_t x) {
	return x;
}

int64_t Widen32(int32_t x) {
	return x;
}

int32_t Negate32(int32_t x) {
	return -x;
}

uint32_t Max32() {
	return 0xFFFFFFFFu;
}

void Touch(int64_t* p) {
	*p = 42;
}

float Half(float x) {
	return x * 0.5f;
}

double Twice(double x) {
	return x * 2.0;
}

enum class Color : uint8_t { Red = 1, Green = 2, Blue = 3 };

Color NextColor(Color c) {
	return c == Color::Blue ? Color::Red : static_cast<Color>(static_cast<uint8_t>(c) + 1);
}

enum Level : int32_t { Low = -1, High = 1 };

Level PickLevel(Level l) {
	return l;
}

char Ch() {
	return static_cast<char>(0xFF);
}

int32_t ReadRef(const int32_t& x) {
	return x + 1;
}

int32_t& Slot(int32_t& x) {
	return x;
}

struct Pair {
	int32_t a;
	int32_t b;
};

int32_t SumPair(Pair p) {
	return p.a + p.b;
}

// Same shape as ShaderRegister / AgcDcbSet*RegisterDirect: 8 bytes of integers.
struct Reg8 {
	uint32_t offset;
	uint32_t value;
};

int64_t WriteReg(int64_t* buf, Reg8 reg, int64_t tail) {
	return buf[0] + reg.offset * 100 + reg.value * 1000 + tail * 100000;
}

// Same shape as AudioOut2Position: three floats, so two SSE eightbytes ({x, y} and {z}).
struct Vec3 {
	float x;
	float y;
	float z;
};

// Parameter order matches AudioOut2GetSpeakerArrayCoefficients: ints in RDI, RSI, RDX, RCX; pos in
// XMM0 (x, y) and XMM1 (z); spread in XMM2; radius in XMM3.
double Weigh(int32_t handle, Vec3 pos, float spread, float* out, uint32_t n, uint8_t height,
             float radius) {
	[[maybe_unused]] float* sink = out;
	return handle * 1.0 + pos.x * 2.0 + pos.y * 4.0 + pos.z * 8.0 + spread * 16.0 + n * 32.0 +
	       height * 64.0 + radius * 128.0;
}

// Two INTEGER eightbytes (16 bytes): 2 registers when they fit, else two stack slots.
struct Quad {
	int64_t p;
	int64_t q;
};

int64_t SumQuadAndTail(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, Quad quad,
                       int64_t tail) {
	return a + b + c + d + e + quad.p * 10 + quad.q * 100 + tail * 1000;
}

// {double, int64}: one SSE eightbyte and one INTEGER eightbyte.
struct Mix {
	double  d;
	int64_t i;
};

double UseMix(Mix m, int64_t k) {
	return m.d * 10.0 + static_cast<double>(m.i) + static_cast<double>(k);
}

// 24 bytes: MEMORY class, passed in three stack slots.
struct Big {
	int64_t a;
	int64_t b;
	int64_t c;
};

int64_t BigSum(Big big, int64_t x) {
	return big.a + big.b * 10 + big.c * 100 + x * 1000;
}

// Returns {size, align}: two INTEGER eightbytes, returned in RAX and RDX.
struct SizeAlignLike {
	uint64_t m_size;
	size_t   m_align;
};

SizeAlignLike MakeSizeAlign(uint64_t size) {
	return {size, 16};
}

Mix MakeMix(double d) {
	return {d * 2.0, 9};
}

Big MakeBig(int64_t base) {
	return {base, base + 1, base + 2};
}

// Variadic-style helpers: only the register/stack image matters; the formatting is done by the
// ctx consumers below.
int32_t OptedOut(int32_t x) {
	return x + 1;
}

} // namespace GuestAbiTestFns

// Aggregate eightbyte classes (SysV 3.2.3), declared the way the project declares its own structs.
template <>
struct Loader::GuestAbi::HleAggregate<GuestAbiTestFns::Pair> {
	static constexpr std::array<Loader::GuestAbi::ArgClass, 1> kEightbytes {
	    Loader::GuestAbi::ArgClass::Integer};
};
template <>
struct Loader::GuestAbi::HleAggregate<GuestAbiTestFns::Reg8> {
	static constexpr std::array<Loader::GuestAbi::ArgClass, 1> kEightbytes {
	    Loader::GuestAbi::ArgClass::Integer};
};
template <>
struct Loader::GuestAbi::HleAggregate<GuestAbiTestFns::Vec3> {
	static constexpr std::array<Loader::GuestAbi::ArgClass, 2> kEightbytes {
	    Loader::GuestAbi::ArgClass::Sse, Loader::GuestAbi::ArgClass::Sse};
};
template <>
struct Loader::GuestAbi::HleAggregate<GuestAbiTestFns::Quad> {
	static constexpr std::array<Loader::GuestAbi::ArgClass, 2> kEightbytes {
	    Loader::GuestAbi::ArgClass::Integer, Loader::GuestAbi::ArgClass::Integer};
};
template <>
struct Loader::GuestAbi::HleAggregate<GuestAbiTestFns::Mix> {
	static constexpr std::array<Loader::GuestAbi::ArgClass, 2> kEightbytes {
	    Loader::GuestAbi::ArgClass::Sse, Loader::GuestAbi::ArgClass::Integer};
};
template <>
struct Loader::GuestAbi::HleAggregate<GuestAbiTestFns::SizeAlignLike> {
	static constexpr std::array<Loader::GuestAbi::ArgClass, 2> kEightbytes {
	    Loader::GuestAbi::ArgClass::Integer, Loader::GuestAbi::ArgClass::Integer};
};

// Variadic functions cannot be marshaled generically; this one opts out to test the flag itself.
template <>
struct Loader::GuestAbi::HleManualThunk<&GuestAbiTestFns::OptedOut> : std::true_type {};

namespace {

using Loader::GuestAbi::CallFrame;
using Loader::GuestAbi::HleThunk;
using Loader::GuestAbi::HleThunkFor;
using Loader::GuestAbi::InvokeHle;
using Loader::GuestAbi::Xmm;
namespace Fns = GuestAbiTestFns;

class FakeCallFrame final : public CallFrame {
public:
	std::array<uint64_t, 6> ints {};
	std::array<Xmm, 8>      vecs {};
	std::array<uint64_t, 8> stack {};

	bool     rax_set  = false;
	uint64_t rax      = 0;
	bool     rdx_set  = false;
	uint64_t rdx      = 0;
	bool     xmm0_set = false;
	Xmm      xmm0 {};

	[[nodiscard]] uint64_t IntArg(int index) const override {
		return ints[static_cast<std::size_t>(index)];
	}
	[[nodiscard]] Xmm VecArg(int index) const override {
		return vecs[static_cast<std::size_t>(index)];
	}
	[[nodiscard]] uint64_t StackArg(int index) const override {
		return stack[static_cast<std::size_t>(index)];
	}
	void SetIntReturn(uint64_t value) override {
		rax     = value;
		rax_set = true;
	}
	void SetIntReturn2(uint64_t value) override {
		rdx     = value;
		rdx_set = true;
	}
	void SetVecReturn(Xmm value) override {
		xmm0     = value;
		xmm0_set = true;
	}
	[[nodiscard]] uint64_t StackArgAddress(int index) const override {
		return reinterpret_cast<uint64_t>(&stack[static_cast<std::size_t>(index)]);
	}

	// Places values in declaration order: the first six in the integer registers, the rest in
	// stack slots.
	template <std::size_t N>
	void LoadIntArgs(const std::array<int64_t, N>& values) {
		for (std::size_t k = 0; k < N; ++k) {
			const auto raw = static_cast<uint64_t>(values[k]);
			if (k < ints.size()) {
				ints[k] = raw;
			} else {
				stack[k - ints.size()] = raw;
			}
		}
	}
};

void Check(bool condition, const char* text, int line) {
	if (!condition) {
		std::fprintf(stderr, "GuestAbiTests:%d: %s\n", line, text);
		std::abort();
	}
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

Xmm F32(float value) {
	return Xmm {std::bit_cast<uint32_t>(value), 0};
}

Xmm F64(double value) {
	return Xmm {std::bit_cast<uint64_t>(value), 0};
}

template <std::size_t N>
using Int64Of = int64_t;

template <std::size_t... I>
void CheckDigitsArity(std::index_sequence<I...> /*seq*/) {
	constexpr std::size_t kCount = sizeof...(I);
	std::array<int64_t, kCount>  values {static_cast<int64_t>(I + 1)...};
	int64_t                      expected = 0;
	for (int64_t v: values) {
		expected = expected * 10 + v;
	}

	FakeCallFrame frame;
	frame.LoadIntArgs(values);
	InvokeHle<&Fns::Digits<Int64Of<I>...>>(frame);
	CHECK(frame.rax == static_cast<uint64_t>(expected));
	CHECK(!frame.rdx_set);
	CHECK(!frame.xmm0_set);
}

template <std::size_t... N>
void CheckAllIntArities(std::index_sequence<N...> /*seq*/) {
	(CheckDigitsArity(std::make_index_sequence<N>{}), ...);
}

void TestIntegerArities() {
	// 0..9 integer arguments: arguments 7..9 are spilled to the stack.
	CheckAllIntArities(std::make_index_sequence<10> {});

	// The thunk pointer behaves like a direct InvokeHle call.
	FakeCallFrame frame;
	frame.LoadIntArgs(std::array<int64_t, 9> {1, 2, 3, 4, 5, 6, 7, 8, 9});
	const HleThunk thunk = HleThunkFor<&Fns::Digits<Int64Of<0>, Int64Of<1>, Int64Of<2>,
	                                                  Int64Of<3>, Int64Of<4>, Int64Of<5>,
	                                                  Int64Of<6>, Int64Of<7>, Int64Of<8>>>();
	CHECK(thunk != nullptr);
	thunk(frame);
	CHECK(frame.rax == 123456789u);
}

void TestDoublesSpill() {
	FakeCallFrame frame;
	for (std::size_t k = 0; k < 8; ++k) {
		frame.vecs[k] = F64(static_cast<double>(k + 1));
	}
	frame.stack[0] = std::bit_cast<uint64_t>(9.0);
	InvokeHle<&Fns::Sum9d>(frame);
	CHECK(std::bit_cast<double>(frame.xmm0.lo) == 45.0);
}

void TestMixedClassOrdering() {
	// Integer args use RDI, RSI, RDX; float args use XMM0, XMM1 (float in the low 32 bits).
	FakeCallFrame frame;
	frame.ints[0] = 1;
	frame.vecs[0] = F64(2.0);
	frame.ints[1] = 3;
	frame.vecs[1] = F32(4.0f);
	frame.ints[2] = 5;
	InvokeHle<&Fns::Mixed>(frame);
	CHECK(std::bit_cast<double>(frame.xmm0.lo) == 129.0); // 1 + 2*2 + 3*4 + 4*8 + 5*16
	CHECK(!frame.rax_set);
}

void TestMixedSpill() {
	FakeCallFrame frame;
	frame.ints = {1, 2, 3, 4, 5, 6};
	for (std::size_t k = 0; k < 8; ++k) {
		frame.vecs[k] = F64(static_cast<double>(k + 1)); // only d0 and d7 are used by Spill17
	}
	frame.stack[0] = 7;                           // s0: first stack slot (int regs exhausted)
	frame.stack[1] = std::bit_cast<uint64_t>(9.0); // d8: XMM regs exhausted, next slot
	frame.stack[2] = 2;                           // s1: next slot after d8
	InvokeHle<&Fns::Spill17>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == 2978161);
}

void TestPointers() {
	char    text[] = {'Z', 0};
	int32_t out    = 0;
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(text);
	frame.ints[1] = reinterpret_cast<uintptr_t>(&out);
	InvokeHle<&Fns::PickPtr>(frame);
	CHECK(out == 'Z');
	CHECK(frame.rax == reinterpret_cast<uintptr_t>(&out + 1));
}

void TestBoolIgnoresUpperBits() {
	FakeCallFrame frame;
	frame.ints[0] = 0xFFFFFF00u; // low byte 0 -> false
	InvokeHle<&Fns::Flip>(frame);
	CHECK(frame.rax == 1);

	frame.rax_set = false;
	frame.ints[0] = 0x100000001ull; // low byte 1 -> true
	InvokeHle<&Fns::Flip>(frame);
	CHECK(frame.rax == 0);
}

void TestNarrowIntegers() {
	FakeCallFrame frame;
	frame.ints[0] = 0xABCDEF01u; // uint8 truncates to 0x01
	InvokeHle<&Fns::Inc8>(frame);
	CHECK(frame.rax == 2);

	frame.ints[0] = 0xFFu; // uint8 0xFF + 1 wraps to 0, zero-extended
	InvokeHle<&Fns::Inc8>(frame);
	CHECK(frame.rax == 0);

	frame.ints[0] = 0x80; // int8 -128 sign-extended to 64 bits
	InvokeHle<&Fns::Id8>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == -128);

	frame.ints[0] = 0x00000000FFFFFFFFull; // int32 -1 from the low 32 bits
	InvokeHle<&Fns::Widen32>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == -1);

	frame.ints[0] = 5;
	InvokeHle<&Fns::Negate32>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == -5); // int32 return sign-extended

	InvokeHle<&Fns::Max32>(frame);
	CHECK(frame.rax == 0x00000000FFFFFFFFull); // uint32 return zero-extended
}

void TestOptOutFlag() {
	static_assert(Loader::GuestAbi::HleManualThunk<&Fns::OptedOut>::value);
	CHECK(HleThunkFor<&Fns::OptedOut>() == nullptr);
	CHECK(HleThunkFor<&Fns::Half>() != nullptr);
	CHECK(HleThunkFor<&Fns::SumPair>() != nullptr);
}

void TestVoidReturn() {
	int64_t       value = 0;
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(&value);
	InvokeHle<&Fns::Touch>(frame);
	CHECK(value == 42);
	CHECK(!frame.rax_set);
	CHECK(!frame.xmm0_set);
}

void TestFloatingReturns() {
	FakeCallFrame frame;
	frame.vecs[0] = F32(3.0f);
	InvokeHle<&Fns::Half>(frame);
	CHECK(static_cast<uint32_t>(frame.xmm0.lo) == std::bit_cast<uint32_t>(1.5f));
	CHECK(!frame.rax_set);

	frame.vecs[0] = F64(1.25);
	InvokeHle<&Fns::Twice>(frame);
	CHECK(std::bit_cast<double>(frame.xmm0.lo) == 2.5);
}

void TestEnums() {
	FakeCallFrame frame;
	frame.ints[0] = 0xFFFFFF02u; // Color is uint8: 0x02 is Green
	InvokeHle<&Fns::NextColor>(frame);
	CHECK(frame.rax == 3);

	frame.ints[0] = 0xFFFFFFFFu; // Level is int32: -1 is Low
	InvokeHle<&Fns::PickLevel>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == -1);
}

void TestCharReturnIsGuestSigned() {
	FakeCallFrame frame;
	InvokeHle<&Fns::Ch>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == -1);
}

void TestReferences() {
	int32_t       value = 41;
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(&value);
	InvokeHle<&Fns::ReadRef>(frame);
	CHECK(frame.rax == 42);

	InvokeHle<&Fns::Slot>(frame);
	CHECK(frame.rax == reinterpret_cast<uintptr_t>(&value));
}

#if defined(__SIZEOF_INT128__)
unsigned __int128 Wide() {
	return (static_cast<unsigned __int128>(0x1122334455667788ull) << 64) | 0x99aabbccddeeff00ull;
}

void TestWideReturn() {
	FakeCallFrame frame;
	InvokeHle<&Wide>(frame);
	CHECK(frame.rax == 0x99aabbccddeeff00ull);
	CHECK(frame.rdx == 0x1122334455667788ull);
}
#endif

void TestPairByValue() {
	// Pair is one INTEGER eightbyte: it takes RDI.
	FakeCallFrame frame;
	frame.ints[0] = static_cast<uint64_t>(static_cast<uint32_t>(-4)) |
	                (static_cast<uint64_t>(10) << 32); // a = -4 in the low half, b = 10
	InvokeHle<&Fns::SumPair>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == 6);
}

void TestRegisterAggregateInteger() {
	// WriteReg(buf, Reg8 reg, tail): buf -> RDI, reg -> RSI, tail -> RDX.
	int64_t       buf[1] = {7};
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(buf);
	frame.ints[1] = (static_cast<uint64_t>(5) << 32) | 3u; // offset = 3, value = 5
	frame.ints[2] = 2;
	InvokeHle<&Fns::WriteReg>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == 7 + 300 + 5000 + 200000);
}

void TestAggregateSseEightbytes() {
	// Vec3 pos takes XMM0 (x, y packed in its low eightbyte) and XMM1 (z); spread XMM2; radius XMM3.
	// Integer args: handle RDI, out RSI, n RDX, height RCX.
	FakeCallFrame frame;
	frame.ints[0] = 1;
	frame.ints[1] = 0;
	frame.ints[2] = 3;
	frame.ints[3] = 1;
	frame.vecs[0] = Xmm {static_cast<uint64_t>(std::bit_cast<uint32_t>(1.0f)) |
	                         (static_cast<uint64_t>(std::bit_cast<uint32_t>(2.0f)) << 32),
	                     0};
	frame.vecs[1] = F32(3.0f);
	frame.vecs[2] = F32(0.5f);
	frame.vecs[3] = F32(0.25f);
	InvokeHle<&Fns::Weigh>(frame);
	// 1 + 2*1 + 4*2 + 8*3 + 16*0.5 + 32*3 + 64*1 + 128*0.25
	CHECK(std::bit_cast<double>(frame.xmm0.lo) == 235.0);
	CHECK(!frame.rax_set);
}

void TestAggregateSpillsWhenRegistersRunOut() {
	// Quad needs two INTEGER registers but only one is left (RDI..RCX hold a..e minus one), so Quad
	// goes to stack slots 0 and 1; tail still takes the last free register (R9).
	FakeCallFrame frame;
	frame.ints = {1, 2, 3, 4, 5, 0};
	frame.stack[0] = 6; // quad.p
	frame.stack[1] = 7; // quad.q
	frame.ints[5]  = 8; // tail
	InvokeHle<&Fns::SumQuadAndTail>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == 15 + 60 + 700 + 8000);
}

void TestMixedAggregateClasses() {
	// Mix: d in XMM0, i in RDI; k in RSI.
	FakeCallFrame frame;
	frame.vecs[0] = F64(1.5);
	frame.ints[0] = 4;
	frame.ints[1] = 2;
	InvokeHle<&Fns::UseMix>(frame);
	CHECK(std::bit_cast<double>(frame.xmm0.lo) == 15.0 + 4.0 + 2.0);
}

void TestMemoryAggregateOnStack() {
	// Big (24 bytes) is MEMORY class: stack slots 0..2, no register; x still takes RDI.
	FakeCallFrame frame;
	frame.ints[0] = 100;
	frame.stack   = {1, 2, 3, 0, 0, 0, 0, 0};
	InvokeHle<&Fns::BigSum>(frame);
	CHECK(static_cast<int64_t>(frame.rax) == 100000 + 1 + 20 + 300);
}

void TestAggregateReturnInRegisters() {
	FakeCallFrame frame;
	frame.ints[0] = 77;
	InvokeHle<&Fns::MakeSizeAlign>(frame);
	CHECK(frame.rax == 77);
	CHECK(frame.rdx_set && frame.rdx == 16);
	CHECK(!frame.xmm0_set);

	frame.rdx_set = false;
	frame.vecs[0] = F64(2.5);
	InvokeHle<&Fns::MakeMix>(frame);
	CHECK(std::bit_cast<double>(frame.xmm0.lo) == 5.0); // d * 2 in XMM0
	CHECK(frame.rax == 9);                             // i in RAX
	CHECK(!frame.rdx_set);
}

void TestAggregateReturnHiddenPointer() {
	// MakeBig returns through a hidden pointer in RDI; base is RSI.
	Fns::Big out {};
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(&out);
	frame.ints[1] = 5;
	InvokeHle<&Fns::MakeBig>(frame);
	CHECK(out.a == 5 && out.b == 6 && out.c == 7);
	CHECK(frame.rax == reinterpret_cast<uintptr_t>(&out)); // RAX echoes the hidden pointer
	CHECK(!frame.rdx_set);
	CHECK(!frame.xmm0_set);
}

// Variadic consumers. They read their arguments the way the guest-side printf/snprintf does: a
// format-driven walk over the VaList built by BuildVaContext.
std::string FormatVa(Libs::VaList* list, const char* fmt) {
	std::string text;
	for (const char* p = fmt; *p != '\0'; ++p) {
		if (*p != '%') {
			text += *p;
			continue;
		}
		++p;
		char tmp[64] = {};
		switch (*p) {
		case 'd':
			text += std::to_string(Libs::VaArg_int(list));
			break;
		case 'l':
			++p; // %ld
			text += std::to_string(Libs::VaArg_long(list));
			break;
		case 's':
			text += Libs::VaArg_ptr<const char>(list);
			break;
		case 'f':
			std::snprintf(tmp, sizeof(tmp), "%.2f", Libs::VaArg_double(list));
			text += tmp;
			break;
		default:
			text += *p;
			break;
		}
	}
	return text;
}

std::string g_printed;

int MiniPrintf(Libs::VaContext* ctx) {
	const char* fmt = Libs::VaArg_ptr<const char>(&ctx->va_list);
	g_printed       = FormatVa(&ctx->va_list, fmt);
	return static_cast<int>(g_printed.size());
}

int MiniSnprintf(Libs::VaContext* ctx) {
	char*       buf = Libs::VaArg_ptr<char>(&ctx->va_list);
	std::size_t n   = Libs::VaArg_size_t(&ctx->va_list);
	const char* fmt = Libs::VaArg_ptr<const char>(&ctx->va_list);
	std::string text = FormatVa(&ctx->va_list, fmt);
	if (n > 0) {
		const std::size_t count = std::min(text.size(), n - 1);
		std::memcpy(buf, text.data(), count);
		buf[count] = '\0';
	}
	return static_cast<int>(text.size());
}

void TestPrintfVariadicRegisters() {
	// printf("%d %s %f", 5, "abc", 2.5): fmt RDI, 5 RSI, "abc" RDX, 2.5 XMM0.
	const char text[] = "abc";
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>("%d %s %f");
	frame.ints[1] = 5;
	frame.ints[2] = reinterpret_cast<uintptr_t>(text);
	frame.vecs[0] = F64(2.5);
	Libs::SetIntResult(frame, Libs::RunVariadic(frame, MiniPrintf));
	CHECK(g_printed == "5 abc 2.50");
	CHECK(frame.rax == 10);
}

void TestPrintfVariadicStackSpill() {
	// Six integer registers hold fmt and five values; the sixth and seventh go to stack slots 0 and 1.
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>("%d %d %d %d %d %d %d");
	frame.ints[1] = 1;
	frame.ints[2] = 2;
	frame.ints[3] = 3;
	frame.ints[4] = 4;
	frame.ints[5] = 5;
	frame.stack[0] = 6;
	frame.stack[1] = 7;
	Libs::SetIntResult(frame, Libs::RunVariadic(frame, MiniPrintf));
	CHECK(g_printed == "1 2 3 4 5 6 7");

	// Doubles: XMM0..XMM7 hold the first eight; the ninth is stack slot 0 (shared with integers).
	FakeCallFrame doubles;
	doubles.ints[0] = reinterpret_cast<uintptr_t>("%f %f %f %f %f %f %f %f %f");
	for (std::size_t k = 0; k < 8; ++k) {
		doubles.vecs[k] = F64(static_cast<double>(k + 1));
	}
	doubles.stack[0] = std::bit_cast<uint64_t>(9.0);
	Libs::SetIntResult(doubles, Libs::RunVariadic(doubles, MiniPrintf));
	CHECK(g_printed == "1.00 2.00 3.00 4.00 5.00 6.00 7.00 8.00 9.00");
}

void TestSnprintfVariadic() {
	// snprintf(buf, 16, "%d:%s:%ld", 42, "xy", -7): buf RDI, size RSI, fmt RDX, 42 RCX, "xy" R8, -7 R9.
	char          buf[16] = {};
	const char    text[]  = "xy";
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(buf);
	frame.ints[1] = sizeof(buf);
	frame.ints[2] = reinterpret_cast<uintptr_t>("%d:%s:%ld");
	frame.ints[3] = 42;
	frame.ints[4] = reinterpret_cast<uintptr_t>(text);
	frame.ints[5] = static_cast<uint64_t>(-7);
	Libs::SetIntResult(frame, Libs::RunVariadic(frame, MiniSnprintf));
	CHECK(std::strcmp(buf, "42:xy:-7") == 0);
	CHECK(frame.rax == 8);
}

void TestSnprintfVariadicStackSpill() {
	// snprintf(buf, 32, "%d %d %d %d", 1, 2, 3, 4): the fourth value spills to stack slot 0.
	char          buf[32] = {};
	FakeCallFrame frame;
	frame.ints[0] = reinterpret_cast<uintptr_t>(buf);
	frame.ints[1] = sizeof(buf);
	frame.ints[2] = reinterpret_cast<uintptr_t>("%d %d %d %d");
	frame.ints[3] = 1;
	frame.ints[4] = 2;
	frame.ints[5] = 3;
	frame.stack[0] = 4;
	Libs::SetIntResult(frame, Libs::RunVariadic(frame, MiniSnprintf));
	CHECK(std::strcmp(buf, "1 2 3 4") == 0);
}

void TestVaContextMatchesVaArgs() {
	// Overflow area is stack slot 0 exactly, the gp/fp save areas hold the raw registers, and
	// reads consume them in order.
	FakeCallFrame frame;
	frame.ints = {11, 12, 13, 14, 15, 16};
	frame.vecs[7] = F64(7.5);
	frame.stack[0] = 17;
	alignas(16) Libs::VaContext ctx {};
	Libs::BuildVaContext(frame, ctx);
	CHECK(ctx.va_list.overflow_arg_area == &frame.stack[0]);
	CHECK(Libs::VaArg_int(&ctx.va_list) == 11);
	for (int expected = 12; expected <= 16; ++expected) {
		CHECK(Libs::VaArg_int(&ctx.va_list) == expected);
	}
	CHECK(Libs::VaArg_int(&ctx.va_list) == 17);
}

void RunAll() {
	TestIntegerArities();
	TestDoublesSpill();
	TestMixedClassOrdering();
	TestMixedSpill();
	TestPairByValue();
	TestRegisterAggregateInteger();
	TestAggregateSseEightbytes();
	TestAggregateSpillsWhenRegistersRunOut();
	TestMixedAggregateClasses();
	TestMemoryAggregateOnStack();
	TestAggregateReturnInRegisters();
	TestAggregateReturnHiddenPointer();
	TestPrintfVariadicRegisters();
	TestPrintfVariadicStackSpill();
	TestSnprintfVariadic();
	TestSnprintfVariadicStackSpill();
	TestVaContextMatchesVaArgs();
	TestPointers();
	TestBoolIgnoresUpperBits();
	TestNarrowIntegers();
	TestVoidReturn();
	TestFloatingReturns();
	TestEnums();
	TestCharReturnIsGuestSigned();
	TestReferences();
#if defined(__SIZEOF_INT128__)
	TestWideReturn();
#endif
	TestOptOutFlag();
}

} // namespace

int main() {
	RunAll();
	std::fprintf(stdout, "GuestAbiTests: all passed\n");
	return 0;
}
