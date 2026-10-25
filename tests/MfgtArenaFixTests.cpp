#include "loader/mfgtArenaFix.h"
#include "loader/mfgtIndexHeader.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace Loader::MfgtArenaFix;

namespace {

void Check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "MfgtArenaFixTests: %s\n", message);
    std::abort();
  }
}

std::vector<uint8_t> SyntheticText() {
  std::vector<uint8_t> text(kTextSize, 0xcc);
  for (const auto &site : kSites) {
    text[site.offset - 2] = site.rex;
    text[site.offset - 1] = site.opcode;
    Detail::Write(text, site.offset, site.before);
  }
  return text;
}

void TestClassifiers() {
  constexpr uint64_t mask = 0x7fffffffff;
  for (uint64_t index = 0; index < 3; ++index) {
    const auto old = kOldBase + index * kArenaSize;
    const auto relocated = kNewBase + index * kArenaSize;
    Check((relocated & mask) >= 0x4400000000 &&
              (relocated & mask) + kArenaSize <= 0x5c00000000,
          "relocated arena escapes the owned ARM64 user window");
    for (auto offset :
         {uint64_t{0}, uint64_t{1}, uint64_t{0x400000}, kArenaSize - 1}) {
      Check(((old + offset - kOldBase) >> 35) == index &&
                ((relocated + offset - kNewBase) >> 35) == index,
            "allocation/free classifier changed arena index");
    }
  }
  Check(((0xe3ffffc000ull - kNewBase) >> 35) >= 3,
        "independent counter page became part of a malloc arena");
  Check(kNewBase + 3 * kArenaSize == 0xdc00000000,
        "relocation changed the aggregate arena capacity");
}

void TestGuardsAndTransaction() {
  auto text = SyntheticText();
  const auto original = text;
  Check(Apply("PPSA10112", true, kEntry, 0, text) == Result::NotApplicable &&
            text == original,
        "unrelated game was changed");
  Check(Apply("PPSA24156", false, kEntry, 0, text) == Result::NotApplicable &&
            text == original,
        "shared module was changed");
  Check(Apply("PPSA24156", true, kEntry, 0x4000, text) ==
                Result::NotApplicable &&
            text == original,
        "nonmatching segment was changed");
  Check(Apply("PPSA24156", true, kEntry + 1, 0, text) ==
                Result::UnsupportedBuild &&
            text == original,
        "different entry was changed");
  Check(Apply("PPSA24156", true, kEntry, 0, text) == Result::UnsupportedBuild &&
            text == original,
        "full executable fingerprint was bypassed");
  text[kSites.back().offset] ^= 1;
  auto corrupt = text;
  Check(!Detail::ApplySites(text) && text == corrupt,
        "late site mismatch left a partial patch");
  text = original;
  text[kSites.back().offset - 1] ^= 1;
  corrupt = text;
  Check(!Detail::ApplySites(text) && text == corrupt,
        "wrong instruction opcode left a partial patch");
  text = original;
  Check(Detail::ApplySites(text), "valid transactional plan rejected");
  for (const auto &site : kSites) {
    Check(Detail::Read(text, site.offset) == site.after &&
              text[site.offset - 2] == site.rex &&
              text[site.offset - 1] == site.opcode,
          "incorrect immediate or changed instruction opcode");
  }
  Check(!Detail::ApplySites(text), "patch applied twice");
  text.resize(0x1000);
  corrupt = text;
  Check(!Detail::ApplySites(text) && text == corrupt,
        "truncated image changed before range validation");
}

void TestIndexHeader() {
  // Synthetic clear header encrypted using the original executable's CRC table.
  const std::array<uint8_t, 72> raw{
      {0x2c, 0xf7, 0x89, 0x4e, 0x5c, 0x52, 0x7a, 0x9d, 0xac, 0xbc, 0xc1, 0x16,
       0x7b, 0xa2, 0x90, 0xea, 0x52, 0x89, 0xed, 0x1f, 0x41, 0xf6, 0x5c, 0xef,
       0x34, 0x86, 0x24, 0xc3, 0x57, 0xbe, 0x66, 0x74, 0x94, 0x60, 0x2f, 0xac,
       0x79, 0xf4, 0xc8, 0x51, 0x66, 0x0b, 0x4c, 0xe5, 0xb4, 0xa2, 0x77, 0xf3,
       0x5d, 0x3e, 0xd8, 0x0c, 0xed, 0x99, 0xe4, 0xe5, 0xe7, 0xe3, 0x99, 0x5a,
       0xc8, 0x1a, 0xa5, 0x28, 0x90, 0xd7, 0x6c, 0x7c, 0xd0, 0xdd, 0xde, 0x33}};
  const auto original = raw;
  const auto result = Loader::MfgtIndexHeader::Decode(raw);
  Check(result.Compatible() && raw == original,
        "index prefix decode or immutability failed");
  for (size_t size = 0; size < raw.size(); ++size)
    Check(!Loader::MfgtIndexHeader::Decode(
               std::span<const uint8_t>(raw).first(size))
               .complete,
          "short index prefix accepted");
  auto damaged = raw;
  damaged[0] ^= 1;
  Check(!Loader::MfgtIndexHeader::Decode(damaged).Compatible(),
        "bad index magic accepted");
  damaged = raw;
  damaged[32] ^= 1;
  Check(!Loader::MfgtIndexHeader::Decode(damaged).Compatible(),
        "bad index version accepted");
  std::array<uint8_t, 72> empty{};
  Check(!Loader::MfgtIndexHeader::Decode(empty).Compatible(),
        "zero-filled index accepted");
}

void TestRealText(const char *path) {
  std::ifstream input(path, std::ios::binary);
  Check(static_cast<bool>(input),
        "cannot read supplied original executable text");
  std::vector<uint8_t> text((std::istreambuf_iterator<char>(input)), {});
  const auto original = text;
  Check(Apply("PPSA24156", true, kEntry, 0, text) == Result::Applied,
        "supplied executable failed exact build guard");
  size_t changed = 0;
  for (size_t i = 0; i < text.size(); ++i)
    changed += text[i] != original[i];
  Check(changed == kSites.size(),
        "unexpected bytes outside 27 allocator immediates changed");
  for (const auto &site : kSites)
    Check(Detail::Read(text, site.offset) == site.after,
          "real executable site not changed");
  std::printf("MfgtArenaFixTests: original executable fingerprint and all 27 "
              "sites verified\n");
}

} // namespace

int main(int argc, char **argv) {
  TestIndexHeader();
  TestClassifiers();
  TestGuardsAndTransaction();
  if (argc == 2)
    TestRealText(argv[1]);
  std::puts("MfgtArenaFixTests: ok");
}
