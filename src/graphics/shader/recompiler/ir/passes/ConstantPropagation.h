#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// wqm_entry_lane_fold: ShaderDeviceFeatures::wqm_entry_lane_fold.
// first_lane_sets: ShaderDeviceFeatures::first_lane_value_sets.
// user_depth: ShaderDeviceFeatures::fold_user_depth (consumer levels requeued per change; 0 = all).
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64,
                             bool wqm_entry_lane_fold = false, bool first_lane_sets = false,
                             uint32_t user_depth = 0);

} // namespace Libs::Graphics::ShaderRecompiler::IR
