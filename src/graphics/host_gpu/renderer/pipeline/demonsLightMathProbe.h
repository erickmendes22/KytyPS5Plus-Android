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
constexpr uint32_t ColorWords = 17;
constexpr uint32_t TermWords = 11;
constexpr uint32_t TermOffset = 1 + ColorWords;
constexpr uint32_t TermStride = 1 + TermWords + 6;
constexpr uint32_t Stride = TermOffset + 2 * TermStride;
constexpr uint32_t WordCount = Samples + Samples * Stride;

// IDs from the captured modules. An exact binary fingerprint is required:
// specializing even one descriptor may renumber intermediate SSA values.
struct TermSite {
	uint32_t anchor, active, shadow, reference;
	std::array<uint32_t, 3> rgb;
	uint32_t weight, visibility, weighted;
	std::array<uint32_t, 3> coordinates;
};

struct Sites {
	uint32_t write_image, x, y;
	std::array<uint32_t, ColorWords> colors;
	std::array<TermSite, 2> terms;
};

inline bool Instrument(std::vector<uint32_t>& spirv, uint64_t fingerprint, uint32_t binding) {
	Sites sites {};
	switch (fingerprint) {
		case 0xda08bbc1750954edull:
			sites = {3727, 269, 277,
			         {422, 315, 291, 3647, 3645, 3646, 1241, 1105, 1109, 1123,
			          3660, 3662, 3664, 3700, 3712, 3706, 1270},
			         {{{2386, 1987, 2313, 2303, {2329, 2331, 2333}, 2347, 2381, 2386,
			            {2274, 2287, 0}},
			           {3555, 3165, 3485, 3476, {3501, 3503, 3505}, 3518, 3550, 3555,
			            {3447, 3460, 0}}}}};
			break;
		case 0xc8fc494909210500ull:
			sites = {3738, 274, 282,
			         {427, 320, 296, 3658, 3656, 3657, 1246, 1110, 1114, 1128,
			          3671, 3673, 3675, 3711, 3723, 3717, 1275},
			         {{{2395, 1992, 2320, 2308, {2338, 2340, 2342}, 2356, 2390, 2395,
			            {2279, 2292, 2259}},
			           {3566, 3174, 3495, 3485, {3512, 3514, 3516}, 3529, 3561, 3566,
			            {3456, 3469, 3437}}}}};
			break;
		case 0x43fbe0f189c71cb4ull:
			sites = {3847, 269, 277,
			         {422, 315, 291, 3767, 3765, 3766, 1241, 1105, 1109, 1123,
			          3780, 3782, 3784, 3820, 3832, 3826, 1270},
			         {{{2446, 2047, 2373, 2363, {2389, 2391, 2393}, 2407, 2441, 2446,
			            {2334, 2347, 0}},
			           {3675, 3285, 3605, 3596, {3621, 3623, 3625}, 3638, 3670, 3675,
			            {3567, 3580, 0}}}}};
			break;
		case 0xf725880048019908ull:
			sites = {3858, 274, 282,
			         {427, 320, 296, 3778, 3776, 3777, 1246, 1110, 1114, 1128,
			          3791, 3793, 3795, 3831, 3843, 3837, 1275},
			         {{{2455, 2052, 2380, 2368, {2398, 2400, 2402}, 2416, 2450, 2455,
			            {2339, 2352, 2319}},
			           {3686, 3294, 3615, 3605, {3632, 3634, 3636}, 3649, 3681, 3686,
			            {3576, 3589, 3557}}}}};
			break;
		case 0x1fe4e93cb1a455b9ull:
			sites = {3722, 274, 282,
			         {427, 320, 296, 3642, 3640, 3641, 1246, 1110, 1114, 1128,
			          3655, 3657, 3659, 3695, 3707, 3701, 1275},
			         {{{2387, 1984, 2312, 2300, {2330, 2332, 2334}, 2348, 2382, 2387,
			            {2271, 2284, 2251}},
			           {3550, 3158, 3479, 3469, {3496, 3498, 3500}, 3513, 3545, 3550,
			            {3440, 3453, 3421}}}}};
			break;
		case 0x3726e6c28a32d6c4ull:
			sites = {3711, 269, 277,
			         {422, 315, 291, 3631, 3629, 3630, 1241, 1105, 1109, 1123,
			          3644, 3646, 3648, 3684, 3696, 3690, 1270},
			         {{{2378, 1979, 2305, 2295, {2321, 2323, 2325}, 2339, 2373, 2378,
			            {2266, 2279, 0}},
			           {3539, 3149, 3469, 3460, {3485, 3487, 3489}, 3502, 3534, 3539,
			            {3431, 3444, 0}}}}};
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
	uint32_t u32 = 0, f32 = 0, boolean = 0, void_type = 0, ptr_word = 0;
	size_t first_type = 0;
	for (size_t i = 5; i < spirv.size();) {
		const size_t count = spirv[i] >> 16;
		if (!count || count > spirv.size() - i) return false;
		const auto op = static_cast<spv::Op>(spirv[i] & 0xffff);
		if (!first_type && op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer)
			first_type = i;
		if (op == spv::OpTypeInt && spirv[i + 2] == 32 && spirv[i + 3] == 0) u32 = spirv[i + 1];
		if (op == spv::OpTypeFloat && spirv[i + 2] == 32) f32 = spirv[i + 1];
		if (op == spv::OpTypeBool) boolean = spirv[i + 1];
		if (op == spv::OpTypeVoid) void_type = spirv[i + 1];
		if (op == spv::OpTypePointer && spirv[i + 2] == spv::StorageClassStorageBuffer &&
		    spirv[i + 3] == u32) ptr_word = spirv[i + 1];
		i += count;
	}
	if (!(first_type && u32 && f32 && boolean && void_type && ptr_word)) return false;

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
	const auto enabled = id();
	emit(declarations, spv::OpConstantTrue, {boolean, enabled});
	const auto recorder_type = [&](uint32_t word_count) {
		const auto type = id();
		declarations.push_back(((6 + word_count) << 16) | spv::OpTypeFunction);
		declarations.insert(declarations.end(), {type, void_type, u32, u32, boolean});
		declarations.insert(declarations.end(), word_count, u32);
		return type;
	};
	const auto make_recorder = [&](uint32_t type, uint32_t offset, uint32_t word_count, bool sum_terms) {
		const auto function = id(), x = id(), y = id(), active = id();
		std::vector<uint32_t> parameters;
		for (uint32_t i = 0; i < word_count; ++i) parameters.push_back(id());
		emit(functions, spv::OpFunction, {void_type, function, 0, type});
		emit(functions, spv::OpFunctionParameter, {u32, x});
		emit(functions, spv::OpFunctionParameter, {u32, y});
		emit(functions, spv::OpFunctionParameter, {boolean, active});
		for (auto parameter : parameters)
			emit(functions, spv::OpFunctionParameter, {u32, parameter});
		emit(functions, spv::OpLabel, {id()});
		const auto shifted = id(), coord = id();
		emit(functions, spv::OpShiftLeftLogical, {u32, shifted, y, constants[16]});
		emit(functions, spv::OpBitwiseOr, {u32, coord, x, shifted});
		for (uint32_t sample = 0; sample < Samples; ++sample) {
			const auto key_ptr = word_ptr(functions, sample), key = id();
			const auto matches = id(), selected = id(), write = id(), merge = id();
			emit(functions, spv::OpLoad, {u32, key, key_ptr});
			emit(functions, spv::OpIEqual, {boolean, matches, key, coord});
			emit(functions, spv::OpLogicalAnd, {boolean, selected, matches, active});
			emit(functions, spv::OpSelectionMerge, {merge, 0});
			emit(functions, spv::OpBranchConditional, {selected, write, merge});
			emit(functions, spv::OpLabel, {write});
			const uint32_t base = Samples + sample * Stride + offset;
			auto pointer = word_ptr(functions, base);
			if (sum_terms) {
				const auto old = id(), increment = id();
				emit(functions, spv::OpLoad, {u32, old, pointer});
				emit(functions, spv::OpIAdd, {u32, increment, old, constants[1]});
				emit(functions, spv::OpStore, {pointer, increment});
				// Preserve sums across the full loop, in addition to keeping the last term.
				for (uint32_t group = 0; group < 2; ++group) {
					const auto weight = id();
					emit(functions, spv::OpBitcast, {f32, weight, parameters[group == 0 ? 5 : 7]});
					for (uint32_t channel = 0; channel < 3; ++channel) {
						const auto color = id(), term = id(), old_bits = id(),
						           old_value = id(), sum = id(), sum_bits = id();
						const auto cell = word_ptr(functions, base + 1 + TermWords + group * 3 + channel);
						emit(functions, spv::OpBitcast, {f32, color, parameters[2 + channel]});
						emit(functions, spv::OpFMul, {f32, term, color, weight});
						emit(functions, spv::OpLoad, {u32, old_bits, cell});
						emit(functions, spv::OpBitcast, {f32, old_value, old_bits});
						emit(functions, spv::OpFAdd, {f32, sum, old_value, term});
						emit(functions, spv::OpBitcast, {u32, sum_bits, sum});
						emit(functions, spv::OpStore, {cell, sum_bits});
					}
				}
			} else {
				emit(functions, spv::OpStore, {pointer, constants[1]});
			}
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
	const auto rgb = make_recorder(recorder_type(ColorWords), 0, ColorWords, false);
	const auto term_type = recorder_type(TermWords);
	const std::array terms {make_recorder(term_type, TermOffset, TermWords, true),
	                        make_recorder(term_type, TermOffset + TermStride, TermWords, true)};
	std::vector<uint32_t> result(spirv.begin(), spirv.begin() + 5);
	bool entry = false, globals = false, color_written = false;
	uint32_t term_sites = 0;
	for (size_t i = 5; i < spirv.size();) {
		const uint32_t count = spirv[i] >> 16;
		const auto op = static_cast<spv::Op>(spirv[i] & 0xffff);
		if (i == first_type) result.insert(result.end(), annotations.begin(), annotations.end());
		if (!globals && op == spv::OpFunction) {
			result.insert(result.end(), declarations.begin(), declarations.end());
			globals = true;
		}
		if (op == spv::OpImageWrite && spirv[i + 1] == sites.write_image) {
			result.push_back(((7 + ColorWords) << 16) | spv::OpFunctionCall);
			result.insert(result.end(), {void_type, id(), rgb, sites.x, sites.y, enabled});
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
		if (op == spv::OpSelect) {
			for (uint32_t slot = 0; slot < terms.size(); ++slot) {
				const auto& term = sites.terms[slot];
				if (spirv[i + 2] != term.anchor) continue;
				const auto shadow_bits = id(), ref_bits = id();
				emit(result, spv::OpBitcast, {u32, shadow_bits, term.shadow});
				emit(result, spv::OpBitcast, {u32, ref_bits, term.reference});
				emit(result, spv::OpFunctionCall,
				     {void_type, id(), terms[slot], sites.x, sites.y, term.active,
				      shadow_bits, ref_bits, term.rgb[0], term.rgb[1], term.rgb[2],
				      term.weight, term.visibility, term.weighted, term.coordinates[0],
				      term.coordinates[1], term.coordinates[2] ? term.coordinates[2] : constants[0]});
				term_sites |= 1u << slot;
			}
		}
		i += count;
	}
	if (!entry || !globals || !color_written || term_sites != 3) return false;
	result.insert(result.end(), functions.begin(), functions.end());
	result[3] = next;
	spirv = std::move(result);
	return true;
}

} // namespace Libs::Graphics::DemonsLightMathProbe
