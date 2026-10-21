#include "graphics/host_gpu/renderer/pipeline/laneBranchDefaults.h"

#include <cstdint>
#include <cstdio>

namespace {

using Libs::Graphics::LaneBranchDefaultShaders;
using Libs::Graphics::LaneBranchesByDefault;

// The list is exact guest hashes: the RT pass only, no prefix or neighbour matches.
static_assert(LaneBranchesByDefault(0x6856320a9c6f89fdull));
static_assert(!LaneBranchesByDefault(0x6856320a9c6f89fcull));
static_assert(!LaneBranchesByDefault(0x6856320a00000000ull));
static_assert(!LaneBranchesByDefault(0));
// Shaders where lane branches were measured without a gain or are not confirmed stay off.
static_assert(!LaneBranchesByDefault(0x3b809f9d156a95ddull)); // G-buffer PS (c14_g2: 1 region)
static_assert(!LaneBranchesByDefault(0x5a10a907c8a64fc4ull)); // intro particle PS

int g_failures = 0;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "LaneBranchDefaultsTests: failed: %s\n", message);
    g_failures++;
  }
}

} // namespace

int main() {
  Check(LaneBranchDefaultShaders.size() == 1, "allow-list grew without a measurement");
  for (const auto hash : LaneBranchDefaultShaders) {
    Check(LaneBranchesByDefault(hash), "listed hash not matched");
  }
  if (g_failures != 0) return 1;
  std::printf("LaneBranchDefaultsTests: ok\n");
  return 0;
}
