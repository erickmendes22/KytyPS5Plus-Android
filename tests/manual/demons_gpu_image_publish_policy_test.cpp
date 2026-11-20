#include "graphics/host_gpu/renderer/cache/gpuImagePublishPolicy.h"
#include <cassert>
using Libs::Graphics::GpuImagePublishPolicy::ChooseAlias;
using Libs::Graphics::GpuImagePublishPolicy::Decision;
static_assert(ChooseAlias(10, 11, true) == Decision::Replace);
static_assert(ChooseAlias(13, 11, true) == Decision::Keep);
static_assert(ChooseAlias(13, 11, false) == Decision::Refuse);
static_assert(ChooseAlias(0, 11, true) == Decision::Refuse);
static_assert(ChooseAlias(11, 0, true) == Decision::Refuse);
static_assert(ChooseAlias(11, 11, true) == Decision::Refuse);
int main() { assert(ChooseAlias(13, 11, true) == Decision::Keep); }
