// --render-scale support in emitted SPIR-V (see renderer/image/renderScale.h). The host keeps
// frame-sized render targets at a fraction of their guest size; a pixel or compute shader that
// addresses pixels directly has to follow:
//   * gl_FragCoord.xy is divided by the scale when the pass's attachments are scaled, so UV
//     math against guest constants and texel fetches at the fragment position stay right;
//   * texel fetches, stores and atomics on an image whose backing is scaled multiply their
//     x/y coordinate by the scale;
//   * size queries of such an image divide their result by the scale (guest size).
// Which images are scaled varies per draw, so the flags, scale and per-image bits come from the
// push data (IR::PushData::Rescale*). With render_scale off nothing here changes the output.

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

namespace {

[[nodiscard]] bool RescaleEnabled(const EmitterState& state) {
	return state.device_features.render_scale && IR::PushData::RescaleStage(state.program.stage);
}

[[nodiscard]] uint32_t RescaleDword(EmitterState& state, uint32_t index) {
	return EmitPushDataLoad(state, ConstantU32(state, index));
}

[[nodiscard]] uint32_t RescaleFactor(EmitterState& state) {
	return Unary(state, spv::OpBitcast, TypeF32(state),
	             RescaleDword(state, IR::PushData::RescaleScaleDword));
}

// Bool: image `resource` of this stage has a scaled backing.
[[nodiscard]] uint32_t ImageScaled(EmitterState& state, uint32_t resource) {
	const auto mask =
	    RescaleDword(state, IR::PushData::RescaleImageMaskDword + (resource >> 5u));
	const auto shifted = Binary(state, spv::OpShiftRightLogical, TypeU32(state), mask,
	                            ConstantU32(state, resource & 31u));
	const auto bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), shifted,
	                        ConstantU32(state, 1u));
	return Binary(state, spv::OpINotEqual, TypeBool(state), bit, ConstantU32(state, 0u));
}

// u32 value scaled by `factor` (multiplied, or divided with rounding to nearest for sizes).
[[nodiscard]] uint32_t ScaleU32(EmitterState& state, uint32_t value, uint32_t factor,
                                bool divide) {
	const auto as_float = Unary(state, spv::OpConvertUToF, TypeF32(state), value);
	auto       scaled   = Binary(state, divide ? spv::OpFDiv : spv::OpFMul, TypeF32(state),
	                             as_float, factor);
	if (divide) {
		scaled = Binary(state, spv::OpFAdd, TypeF32(state), scaled, ConstantF32Value(state, 0.5f));
	}
	return Unary(state, spv::OpConvertFToU, TypeU32(state), scaled);
}

} // namespace

bool NeedsRescaleData(const EmitterState& state) {
	if (!RescaleEnabled(state)) {
		return false;
	}
	const auto& info = state.program.info;
	if (state.program.stage == ShaderType::Pixel &&
	    std::ranges::any_of(info.inputs, [](const IR::StageInput& input) {
		    return input.kind == IR::StageInputKind::FragCoord;
	    })) {
		return true;
	}
	return std::ranges::any_of(info.images,
	                           [](const IR::ImageResource& image) { return image.texel_access; });
}

uint32_t EmitRescaleFragCoord(EmitterState& state, uint32_t value) {
	if (!RescaleEnabled(state) || state.program.stage != ShaderType::Pixel ||
	    state.device_features.render_scale_keep_fragcoord) {
		return value;
	}
	const auto flags = RescaleDword(state, IR::PushData::RescaleFlagsDword);
	const auto bit   = Binary(state, spv::OpBitwiseAnd, TypeU32(state), flags,
	                          ConstantU32(state, IR::PushData::RescaleFlagAttachmentsScaled));
	const auto scaled_pass =
	    Binary(state, spv::OpINotEqual, TypeBool(state), bit, ConstantU32(state, 0u));
	const auto unscaled =
	    Binary(state, spv::OpFDiv, TypeF32(state), value, RescaleFactor(state));
	return Select(state, TypeF32(state), scaled_pass, unscaled, value);
}

uint32_t EmitRescaleTexelCoord(EmitterState& state, uint32_t resource, uint32_t coord,
                               ImageDimension dimension) {
	const auto& info = ImageDimensionInfoFor(dimension);
	// Only 2D (array) images ever get a scaled backing.
	if (!RescaleEnabled(state) || resource >= 64u || info.spatial_components != 2u ||
	    info.coordinate_components < 2u || state.device_features.render_scale_keep_texels) {
		return coord;
	}
	const auto scaled = ImageScaled(state, resource);
	const auto factor = RescaleFactor(state);
	uint32_t   components[3] {};
	for (uint32_t index = 0; index < info.coordinate_components; index++) {
		auto component = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), component, coord,
		                          index);
		if (index < 2u) {
			component = Select(state, TypeU32(state), scaled,
			                   ScaleU32(state, component, factor, false), component);
		}
		components[index] = component;
	}
	const auto result = state.builder.AllocateId();
	if (info.coordinate_components == 3u) {
		state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), result,
		                          components[0], components[1], components[2]);
	} else {
		state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 2), result,
		                          components[0], components[1]);
	}
	return result;
}

uint32_t EmitRescaleQuerySize(EmitterState& state, uint32_t resource, uint32_t size) {
	if (!RescaleEnabled(state) || resource >= 64u ||
	    state.device_features.render_scale_keep_texels) {
		return size;
	}
	return Select(state, TypeU32(state), ImageScaled(state, resource),
	              ScaleU32(state, size, RescaleFactor(state), true), size);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
