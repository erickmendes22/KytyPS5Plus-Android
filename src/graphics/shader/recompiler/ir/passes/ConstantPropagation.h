#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// wqm_entry_lane_fold: ShaderDeviceFeatures::wqm_entry_lane_fold.
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64,
                             bool wqm_entry_lane_fold = false);

} // namespace Libs::Graphics::ShaderRecompiler::IR
