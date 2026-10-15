#include "loader/guestAbiInvoke.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

} // namespace GuestAbiTestFns

// SumPair takes a struct by value, so it cannot be marshaled generically.
template <>
struct Loader::GuestAbi::HleManualThunk<&GuestAbiTestFns::SumPair> : std::true_type {};

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

void TestManualOptOut() {
	static_assert(Loader::GuestAbi::HleManualThunk<&Fns::SumPair>::value);
	CHECK(HleThunkFor<&Fns::SumPair>() == nullptr);
	CHECK(HleThunkFor<&Fns::Half>() != nullptr);
}

void RunAll() {
	TestIntegerArities();
	TestDoublesSpill();
	TestMixedClassOrdering();
	TestMixedSpill();
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
	TestManualOptOut();
}

} // namespace

int main() {
	RunAll();
	std::fprintf(stdout, "GuestAbiTests: all passed\n");
	return 0;
}
