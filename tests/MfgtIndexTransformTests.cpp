#include "loader/mfgtIndexTransform.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
namespace P = Loader::MfgtIndexTransform;
namespace {
void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "MfgtIndexTransformTests: %s\n", message);
    std::abort();
  }
}
std::vector<uint8_t> Read(const char *path) {
  std::ifstream f(path, std::ios::binary);
  Check(bool(f), "input missing");
  return {(std::istreambuf_iterator<char>(f)), {}};
}
uint64_t Hash(std::span<const uint8_t> b) {
  uint64_t h = 14695981039346656037ull;
  for (auto v : b) {
    h ^= v;
    h *= 1099511628211ull;
  }
  return h;
}
void KnownBlock() {
  // Standard 20-round block vector: state counter=1, 256-bit ascending key.
  const P::State state = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                          0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
                          0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
                          1,          0x09000000, 0x4a000000, 0};
  const std::array<uint8_t, 64> expected = {
      0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd,
      0x1f, 0xa3, 0x20, 0x71, 0xc4, 0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0,
      0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e, 0xd2,
      0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05,
      0xd9, 0x8b, 0x02, 0xa2, 0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e,
      0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};
  auto copy = state;
  std::vector<uint8_t> output(64);
  P::Transform(copy, output, 64);
  Check(std::equal(output.begin(), output.end(), expected.begin()),
        "known stream block differs");
  Check(copy[12] == 2, "counter not advanced after exact block");
  std::vector<uint8_t> full(257);
  copy = state;
  P::Transform(copy, full, 0);
  for (size_t start : {size_t{0}, size_t{1}, size_t{63}, size_t{64}, size_t{65},
                       size_t{127}}) {
    std::vector<uint8_t> piece(77);
    copy = state;
    P::Transform(copy, piece, start);
    Check(std::equal(piece.begin(), piece.end(), full.begin() + start),
          "offset/tail stream differs");
    Check(copy[12] == (start + piece.size()) / 64,
          "final partial/exact counter differs");
  }
  copy = state;
  std::vector<uint8_t> empty;
  P::Transform(copy, empty, 130);
  Check(copy[12] == 2, "empty call did not set block counter");
  std::vector<uint8_t> short_header(2047, 0xa5);
  const auto original = short_header;
  Check(!P::TransformHeader(short_header) && short_header == original,
        "short header mutated");
}
void Branches() {
  std::array<uint8_t, 5> call;
  constexpr uint64_t pc = 0x903d4ef3a;
  for (uint64_t target :
       {pc, pc + 0x10000, pc - 0x10000, pc + 5 + uint64_t(INT32_MAX),
        pc + 5 - uint64_t(INT32_MAX) - 1}) {
    Check(P::Call(pc, target, call), "valid near branch rejected");
    const auto displacement =
        static_cast<int32_t>(P::Read32(std::span<const uint8_t>(call), 1));
    Check(call[0] == 0xe8 && uint64_t(int64_t(pc + 5) + displacement) == target,
          "branch lost target or CALL return semantics");
  }
  call.fill(0xcc);
  auto unchanged = call;
  Check(!P::Call(pc, pc + 6 + uint64_t(INT32_MAX), call) && call == unchanged,
        "far branch accepted or partially changed");
  Check(!P::Call(UINT64_MAX, pc, call) && call == unchanged,
        "overflowing branch accepted");
  const auto jump = P::Jump(0x7812345678);
  Check(jump[0] == 0x48 && jump[1] == 0xb8 && jump[10] == 0xff &&
            jump[11] == 0xe0,
        "jump does not tail-call trap");
  uint64_t target = 0;
  for (unsigned i = 0; i < 8; ++i)
    target |= uint64_t(jump[2 + i]) << (8 * i);
  Check(target == 0x7812345678, "trap address truncated");
}
void Real(const char *text_path, const char *state_path,
          const char *index_path) {
  const auto text = Read(text_path);
  const auto raw_state = Read(state_path);
  auto index = Read(index_path);
  Check(text.size() > P::kStreamCall + 5 && raw_state.size() == 64 &&
            index.size() == 194866,
        "real input size differs");
  Check(std::equal(P::kStreamOriginal.begin(), P::kStreamOriginal.end(),
                   text.begin() + P::kStreamCall) &&
            std::equal(P::kHeaderOriginal.begin(), P::kHeaderOriginal.end(),
                       text.begin() + P::kHeaderCall),
        "original calls differ");
  P::State state;
  for (size_t i = 0; i < 16; ++i)
    state[i] = P::Read32(raw_state, i * 4);
  P::Transform(state, index, 0);
  Check(P::Read32(index, 0) == 0x4e89f72c,
        "stream header differs before CRC transform");
  Check(P::TransformHeader(index), "real header too short");
  Check(P::Read32(index, 0) == 0x122bb8f2 &&
            P::Read32(index, 0x20) == 0x11160000,
        "real index magic/version rejected");
  Check(uint64_t(P::Read32(index, 0x24)) + P::Read32(index, 0x28) <=
            index.size(),
        "name index span invalid");
  Check(P::Read32(index, 0x30) % 24 == 0 &&
            uint64_t(P::Read32(index, 0x2c)) + P::Read32(index, 0x30) <=
                index.size(),
        "entry span invalid");
  Check(
      Hash(index) == 0x18853ced4a25d0faull,
      "full decoded index differs from independent vector-semantics reference");
  std::printf("real index decoded: size=%zu fnv=%016llx entries=%u\n",
              index.size(), static_cast<unsigned long long>(Hash(index)),
              P::Read32(index, 0x30) / 24);
}
} // namespace
int main(int argc, char **argv) {
  KnownBlock();
  Branches();
  if (argc == 4)
    Real(argv[1], argv[2], argv[3]);
  std::puts("MfgtIndexTransformTests: ok");
}
