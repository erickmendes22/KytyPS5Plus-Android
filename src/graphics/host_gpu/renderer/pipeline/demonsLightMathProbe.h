#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::DemonsLightMathProbe {

constexpr uint64_t Shader = 0xe3daeb1e7426761bull;
constexpr uint32_t Samples = 5;
constexpr uint32_t Stride = 26;
constexpr uint32_t WordCount = Samples + Samples * Stride;
constexpr uint32_t ColorWords = 17;
constexpr uint32_t ShadowWords = 2;

// IDs from the captured modules. An exact binary fingerprint is required:
// specializing even one descriptor may renumber intermediate SSA values.
struct Sites {
	uint32_t write_image, x, y;
	std::array<uint32_t, ColorWords> colors;
	std::array<uint32_t, 2> shadow_result, shadow_ref;
};

inline bool Instrument(std::vector<uint32_t>& spirv, uint64_t fingerprint, uint32_t binding) {
	Sites sites {};
	switch (fingerprint) {
		case 0xda08bbc1750954edull:
			sites = {3727, 269, 277,
			         {422, 315, 291, 3647, 3645, 3646, 1241, 1105, 1109, 1123,
			          3660, 3662, 3664, 3700, 3712, 3706, 1270},
			         {2313, 3485}, {2303, 3476}};
			break;
		case 0xc8fc494909210500ull:
			sites = {3738, 274, 282,
			         {427, 320, 296, 3658, 3656, 3657, 1246, 1110, 1114, 1128,
			          3671, 3673, 3675, 3711, 3723, 3717, 1275},
			         {2320, 3495}, {2308, 3485}};
			break;
		default: return false;
	}
	if (spirv.size() < 5) return false;
	uint32_t next = spirv[3];
	const auto id = [&] { return next++; };
	const auto emit = [](std::vector<uint32_t>& out, spv::Op op,
	                     std::initializer_list<uint32_t> args) {
		out.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | op);
		out.insert(out.end(), args);
	};
	uint32_t u32 = 0, boolean = 0, void_type = 0, ptr_word = 0;
	size_t first_type = 0;
	for (size_t i = 5; i < spirv.size();) {
		const size_t count = spirv[i] >> 16;
		if (!count || count > spirv.size() - i) return false;
		const auto op = static_cast<spv::Op>(spirv[i] & 0xffff);
		if (!first_type && op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer)
			first_type = i;
		if (op == spv::OpTypeInt && spirv[i + 2] == 32 && spirv[i + 3] == 0) u32 = spirv[i + 1];
		if (op == spv::OpTypeBool) boolean = spirv[i + 1];
		if (op == spv::OpTypeVoid) void_type = spirv[i + 1];
		if (op == spv::OpTypePointer && spirv[i + 2] == spv::StorageClassStorageBuffer &&
		    spirv[i + 3] == u32) ptr_word = spirv[i + 1];
		i += count;
	}
	if (!(first_type && u32 && boolean && void_type && ptr_word)) return false;

	const auto data = id(), block = id(), ptr_block = id(), buffer = id();
	std::array<uint32_t, WordCount> constants {};
	std::vector<uint32_t> annotations, declarations, functions;
	emit(annotations, spv::OpDecorate, {data, spv::DecorationArrayStride, 4});
	emit(annotations, spv::OpDecorate, {block, spv::DecorationBlock});
	emit(annotations, spv::OpMemberDecorate, {block, 0, spv::DecorationOffset, 0});
	emit(annotations, spv::OpDecorate, {buffer, spv::DecorationDescriptorSet, 0});
	emit(annotations, spv::OpDecorate, {buffer, spv::DecorationBinding, binding});
	emit(declarations, spv::OpTypeRuntimeArray, {data, u32});
	emit(declarations, spv::OpTypeStruct, {block, data});
	emit(declarations, spv::OpTypePointer, {ptr_block, spv::StorageClassStorageBuffer, block});
	emit(declarations, spv::OpVariable, {ptr_block, buffer, spv::StorageClassStorageBuffer});
	for (uint32_t i = 0; i < constants.size(); ++i) {
		constants[i] = id();
		emit(declarations, spv::OpConstant, {u32, constants[i], i});
	}
	const auto word_ptr = [&](std::vector<uint32_t>& output, uint32_t slot) {
		const auto ptr = id();
		emit(output, spv::OpAccessChain, {ptr_word, ptr, buffer, constants[0], constants[slot]});
		return ptr;
	};
	const auto recorder_type = [&](uint32_t word_count) {
		const auto type = id();
		declarations.push_back(((5 + word_count) << 16) | spv::OpTypeFunction);
		declarations.insert(declarations.end(), {type, void_type, u32, u32});
		declarations.insert(declarations.end(), word_count, u32);
		return type;
	};
	const auto make_recorder = [&](uint32_t type, uint32_t offset, uint32_t word_count) {
		const auto function = id(), x = id(), y = id();
		std::vector<uint32_t> parameters;
		for (uint32_t i = 0; i < word_count; ++i) parameters.push_back(id());
		emit(functions, spv::OpFunction, {void_type, function, 0, type});
		emit(functions, spv::OpFunctionParameter, {u32, x});
		emit(functions, spv::OpFunctionParameter, {u32, y});
		for (auto parameter : parameters)
			emit(functions, spv::OpFunctionParameter, {u32, parameter});
		emit(functions, spv::OpLabel, {id()});
		const auto shifted = id(), coord = id();
		emit(functions, spv::OpShiftLeftLogical, {u32, shifted, y, constants[16]});
		emit(functions, spv::OpBitwiseOr, {u32, coord, x, shifted});
		for (uint32_t sample = 0; sample < Samples; ++sample) {
			const auto key_ptr = word_ptr(functions, sample), key = id();
			const auto selected = id(), write = id(), merge = id();
			emit(functions, spv::OpLoad, {u32, key, key_ptr});
			emit(functions, spv::OpIEqual, {boolean, selected, key, coord});
			emit(functions, spv::OpSelectionMerge, {merge, 0});
			emit(functions, spv::OpBranchConditional, {selected, write, merge});
			emit(functions, spv::OpLabel, {write});
			const uint32_t base = Samples + sample * Stride + offset;
			auto pointer = word_ptr(functions, base);
			emit(functions, spv::OpStore, {pointer, constants[1]});
			for (uint32_t i = 0; i < parameters.size(); ++i) {
				pointer = word_ptr(functions, base + 1 + i);
				emit(functions, spv::OpStore, {pointer, parameters[i]});
			}
			emit(functions, spv::OpBranch, {merge});
			emit(functions, spv::OpLabel, {merge});
		}
		emit(functions, spv::OpReturn, {});
		emit(functions, spv::OpFunctionEnd, {});
		return function;
	};
	const auto rgb = make_recorder(recorder_type(ColorWords), 0, ColorWords);
	const auto shadow_type = recorder_type(ShadowWords);
	const std::array shadow {make_recorder(shadow_type, 18, ShadowWords),
	                         make_recorder(shadow_type, 22, ShadowWords)};
	std::vector<uint32_t> result(spirv.begin(), spirv.begin() + 5);
	bool entry = false, globals = false, color_written = false;
	uint32_t shadows = 0;
	for (size_t i = 5; i < spirv.size();) {
		const uint32_t count = spirv[i] >> 16;
		const auto op = static_cast<spv::Op>(spirv[i] & 0xffff);
		if (i == first_type) result.insert(result.end(), annotations.begin(), annotations.end());
		if (!globals && op == spv::OpFunction) {
			result.insert(result.end(), declarations.begin(), declarations.end());
			globals = true;
		}
		if (op == spv::OpImageWrite && spirv[i + 1] == sites.write_image) {
			result.push_back(((6 + ColorWords) << 16) | spv::OpFunctionCall);
			result.insert(result.end(), {void_type, id(), rgb, sites.x, sites.y});
			result.insert(result.end(), sites.colors.begin(), sites.colors.end());
			color_written = true;
		}
		if (op == spv::OpEntryPoint) {
			if (entry) return false;
			result.push_back(spirv[i] + (1u << 16));
			result.insert(result.end(), spirv.begin() + i + 1, spirv.begin() + i + count);
			result.push_back(buffer);
			entry = true;
		} else {
			result.insert(result.end(), spirv.begin() + i, spirv.begin() + i + count);
		}
		if (op == spv::OpImageSampleDrefExplicitLod) {
			for (uint32_t s = 0; s < shadow.size(); ++s) {
				if (spirv[i + 2] != sites.shadow_result[s]) continue;
				const auto factor_bits = id(), ref_bits = id();
				emit(result, spv::OpBitcast, {u32, factor_bits, sites.shadow_result[s]});
				emit(result, spv::OpBitcast, {u32, ref_bits, sites.shadow_ref[s]});
				emit(result, spv::OpFunctionCall,
				     {void_type, id(), shadow[s], sites.x, sites.y, factor_bits, ref_bits});
				shadows |= 1u << s;
			}
		}
		i += count;
	}
	if (!entry || !globals || !color_written || shadows != 3) return false;
	result.insert(result.end(), functions.begin(), functions.end());
	result[3] = next;
	spirv = std::move(result);
	return true;
}

} // namespace Libs::Graphics::DemonsLightMathProbe
