#include "graphics/host_gpu/renderer/indirectDispatchPolicy.h"

using Libs::Graphics::ChooseIndirectDispatchSource;
using Libs::Graphics::IndirectDispatchSource;

// No extra work is emitted when the GPU argument record really contains zero;
// only the unsafe early CPU return is suppressed for GPU-authored arguments.
static_assert(ChooseIndirectDispatchSource(true, true, false) == IndirectDispatchSource::Skip);
static_assert(ChooseIndirectDispatchSource(true, true, true) == IndirectDispatchSource::Gpu);
static_assert(ChooseIndirectDispatchSource(false, true, false) == IndirectDispatchSource::Gpu);
static_assert(ChooseIndirectDispatchSource(false, true, true) == IndirectDispatchSource::Gpu);
static_assert(ChooseIndirectDispatchSource(true, false, true) == IndirectDispatchSource::Cpu);
static_assert(ChooseIndirectDispatchSource(true, false, false) == IndirectDispatchSource::Cpu);

int main() { return 0; }
