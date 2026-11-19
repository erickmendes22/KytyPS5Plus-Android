#include "graphics/shader/recompiler/backend/spirv/ldsInt64StoragePolicy.h"

using Libs::Graphics::ShaderRecompiler::Spirv::ShouldUseStorageLdsForInt64;

// DS compute kernel on Turnip: use the existing per-workgroup storage buffer.
static_assert(ShouldUseStorageLdsForInt64(true, true, true, true, false, true));
// Other titles, other stages, kernels with no 64-bit LDS atomic, and devices
// with native shared atomics retain their original shader topology.
static_assert(!ShouldUseStorageLdsForInt64(false, true, true, true, false, true));
static_assert(!ShouldUseStorageLdsForInt64(true, false, true, true, false, true));
static_assert(!ShouldUseStorageLdsForInt64(true, true, false, true, false, true));
static_assert(!ShouldUseStorageLdsForInt64(true, true, true, false, false, true));
static_assert(!ShouldUseStorageLdsForInt64(true, true, true, true, true, true));
static_assert(!ShouldUseStorageLdsForInt64(true, true, true, true, false, false));

int main() { return 0; }
