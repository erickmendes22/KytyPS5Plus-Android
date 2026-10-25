#include "loader/shPerformancePatches.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace P = Loader::ShPerformancePatches;
namespace {
void Check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "ShPerformancePatchesTests: %s\n", message);
    std::abort();
  }
}
std::vector<uint8_t> Read(const char *path) {
  std::ifstream input(path, std::ios::binary);
  Check(bool(input), "cannot read binary segment");
  return {(std::istreambuf_iterator<char>(input)), {}};
}
void Put(std::vector<uint8_t> &bytes, size_t offset, std::string_view hex) {
  for (size_t i = 0; i < hex.size() / 2; ++i)
    bytes[offset + i] = (P::Detail::Nibble(hex[2 * i]) << 4) |
                        P::Detail::Nibble(hex[2 * i + 1]);
}
void ResetSites(std::vector<uint8_t> &text, std::vector<uint8_t> &globals) {
  for (const auto &site : P::Detail::kRegistrations)
    Put(text, site.begin, site.original);
  Put(text, P::Detail::kResolutionBegin, P::Detail::kResolutionOriginal);
  Put(text, P::Detail::kDynamicResBegin, P::Detail::kDynamicResOriginal);
  P::Detail::Write32(globals, P::kFogOffset, 1);
}
void TestCombinations(std::vector<uint8_t> text,
                      const std::vector<uint8_t> &rodata,
                      std::vector<uint8_t> globals) {
  const auto original = text;
  const auto original_globals = globals;
  for (unsigned percent : {0u, 25u, 50u, 75u}) {
    for (unsigned mask = 0; mask < (1u << P::kEffectCount); ++mask) {
      ResetSites(text, globals);
      P::Options options;
      options.screen_percent = percent;
      for (size_t i = 0; i < P::kEffectCount; ++i)
        options.disable[i] = (mask & (1u << i)) != 0;
      Check(P::ApplySites(text, rodata, globals, options),
            "valid combination rejected");
      for (const auto &site : P::Detail::kRegistrations) {
        if (!options.disable[static_cast<size_t>(site.effect)]) {
          Check(P::Detail::Matches(text, site.begin, site.original),
                "unselected effect changed");
          continue;
        }
        if (site.value)
          Check(P::Detail::Read32(text, site.value) == 0,
                "effect default not zero");
        const auto before = P::Detail::Read32(original, site.flags);
        const auto after = P::Detail::Read32(text, site.flags);
        Check(after == ((before & 0xffffffu) | P::kConsolePriority),
              "priority lost original flags");
        for (unsigned source = 0; source < 9; ++source)
          Check((source << 24) < (after & 0xff000000u),
                "lower-priority config can override effect");
      }
      Check(
          P::Detail::Read32(globals, P::kFogOffset) ==
              (options.disable[static_cast<size_t>(P::Effect::Fog)] ? 0u : 1u),
          "fog storage changed incorrectly");
      if (percent) {
        const int32_t disp = static_cast<int32_t>(
            P::Detail::Read32(text, P::Detail::kResolutionDisp));
        Check(int64_t(0x122c03c) + disp ==
                  int64_t(P::Detail::FloatRva(percent)),
              "resolution reference points outside selected float");
        Check(P::Detail::Read32(text, P::Detail::kResolutionFlags) ==
                  0x09000040,
              "screen percentage priority incorrect");
        Check(P::Detail::Read32(text, P::Detail::kDynamicResFlags) ==
                  0x09000020,
              "dynamic resolution can override percentage");
      } else {
        Check(P::Detail::Matches(text, P::Detail::kResolutionBegin,
                                 P::Detail::kResolutionOriginal) &&
                  P::Detail::Matches(text, P::Detail::kDynamicResBegin,
                                     P::Detail::kDynamicResOriginal),
              "effects changed resolution");
      }
    }
  }
  // All writes must stay inside known immediates/displacement and one fog word.
  P::Options all;
  all.screen_percent = 50;
  all.disable.fill(true);
  ResetSites(text, globals);
  Check(P::ApplySites(text, rodata, globals, all), "all-selected patch failed");
  std::vector<size_t> allowed;
  for (const auto &site : P::Detail::kRegistrations) {
    for (size_t i = 0; i < 4; ++i) {
      allowed.push_back(site.flags + i);
      if (site.value)
        allowed.push_back(site.value + i);
    }
  }
  for (const size_t site :
       {P::Detail::kResolutionDisp, P::Detail::kResolutionFlags,
        P::Detail::kDynamicResFlags})
    for (size_t i = 0; i < 4; ++i)
      allowed.push_back(site + i);
  for (size_t i = 0; i < text.size(); ++i)
    if (text[i] != original[i])
      Check(std::find(allowed.begin(), allowed.end(), i) != allowed.end(),
            "write outside an allowed instruction immediate");
  for (size_t i = 0; i < globals.size(); ++i)
    if (globals[i] != original_globals[i])
      Check(i >= P::kFogOffset && i < P::kFogOffset + 4,
            "write outside fog integer");
  ResetSites(text, globals);
  // A late mismatch must reject the entire combined selection, before any
  // write.
  text[P::Detail::kRegistrations.back().begin] ^= 1;
  auto damaged = text;
  Check(!P::ApplySites(text, rodata, globals, all) && text == damaged &&
            globals == original_globals,
        "late mismatch left partial patches");
  text = original;
  globals[P::kFogOffset] = 2;
  auto damaged_globals = globals;
  Check(!P::ApplySites(text, rodata, globals, all) && text == original &&
            globals == damaged_globals,
        "fog mismatch left a partial code patch");
  globals = original_globals;
  all.screen_percent = 33;
  Check(!P::ApplySites(text, rodata, globals, all) && text == original &&
            globals == original_globals,
        "invalid resolution accepted");
  Check(!P::ValidateSites(std::span<const uint8_t>(text).first(10), rodata,
                          globals, all),
        "truncated text accepted");
}
} // namespace
int main(int argc, char **argv) {
  std::vector<uint8_t> text(P::kTextSize, 0xcc), rodata(P::kReadOnlySize, 0),
      globals(P::kGlobalsSize, 0);
  ResetSites(text, globals);
  P::Detail::Write32(rodata, P::Detail::kOriginalFloatRva - P::kReadOnlyRva,
                     0x42c80000);
  for (unsigned percent : {25u, 50u, 75u})
    P::Detail::Write32(rodata, P::Detail::FloatRva(percent) - P::kReadOnlyRva,
                       P::Detail::FloatBits(percent));
  TestCombinations(text, rodata, globals);
  if (argc == 4) {
    text = Read(argv[1]);
    rodata = Read(argv[2]);
    globals = Read(argv[3]);
    Check(P::MatchesIdentity("PPSA10112", true, P::kEntry, text),
          "supplied executable does not match full identity");
    Check(!P::MatchesIdentity("PPSA24156", true, P::kEntry, text) &&
              !P::MatchesIdentity("PPSA10112", false, P::kEntry, text) &&
              !P::MatchesIdentity("PPSA10112", true, P::kEntry + 1, text),
          "title/module/entry guard failed");
    auto changed = text;
    changed[0x10000] ^= 1;
    Check(!P::MatchesIdentity("PPSA10112", true, P::kEntry, changed),
          "changed executable passed fingerprint");
    TestCombinations(text, rodata, globals);
    std::puts("ShPerformancePatchesTests: real executable, all 1024 "
              "combinations verified");
  }
  std::puts("ShPerformancePatchesTests: ok");
}
