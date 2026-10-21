#include "graphics/shader/recompiler/frontend/translate/HalfRateTiles.h"

#include "graphics/shader/halfRateTiles.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

namespace H = Libs::Graphics::HalfRateTiles;
using Decoder::Opcode;
using Decoder::OperandKind;

// LDS arrays of kSamples words each (graphics/shader/halfRateTiles.h).
enum Array : uint32_t {
	Valid   = 0,
	Depth   = 1,
	Gbuffer = 2, // 4 arrays
	Color   = 6, // 4 arrays
	Luma    = 10,
};
static_assert(Luma + 1 == H::kArrays);

// Weights of the joint bilateral upsample: relative linear-depth difference (standard Z, far = 1,
// so 1 - depth is proportional to 1 / view depth) and normalized G-buffer difference.
constexpr float kDepthSharpness   = 16.0f;
constexpr float kGbufferSharpness = 4.0f;
constexpr float kWeightFloor      = 1e-6f;
constexpr uint32_t kTileSize      = 16;
constexpr uint32_t kLastSample    = kTileSize - 2u;

const Decoder::Instruction* FindPc(const Decoder::Program& decoded, uint32_t pc) {
	const auto it = std::ranges::find_if(decoded.instructions,
	                                     [pc](const Decoder::Instruction& inst) { return inst.pc == pc; });
	return it == decoded.instructions.end() ? nullptr : &*it;
}

bool IsVgpr(const Decoder::Operand& operand, uint32_t reg) {
	return operand.kind == OperandKind::Vgpr && operand.reg == reg;
}

bool IsSgpr(const Decoder::Operand& operand, uint32_t reg) {
	return operand.kind == OperandKind::Sgpr && operand.reg == reg;
}

struct Expect {
	uint32_t pc;
	Opcode   opcode;
	bool (*shape)(const Decoder::Instruction&);
};

bool DescLoad(const Decoder::Instruction& inst, uint32_t dst, uint32_t offset) {
	return IsSgpr(inst.dst, dst) && inst.offset == offset && IsSgpr(inst.src0, 14);
}

const std::array<Expect, 12> kExpected {{
    {H::kPcCoordX, Opcode::V_ADD_NC_U32, [](const auto& i) { return IsVgpr(i.dst, 24); }},
    {H::kPcCoordY, Opcode::V_ADD_NC_U32, [](const auto& i) { return IsVgpr(i.dst, 25); }},
    {H::kPcBoundsCmp, Opcode::V_CMP_LE_U32, [](const auto&) { return true; }},
    {H::kPcDepthDesc, Opcode::S_LOAD_DWORDX8, [](const auto& i) { return DescLoad(i, 16, 128); }},
    {H::kPcGbufferDesc, Opcode::S_LOAD_DWORDX8, [](const auto& i) { return DescLoad(i, 24, 0); }},
    {H::kPcDepthLoad, Opcode::IMAGE_LOAD,
     [](const auto& i) { return IsVgpr(i.dst, 26) && IsVgpr(i.src0, 24) && i.dmask == 0x1u; }},
    {H::kPcGbufferLoad, Opcode::IMAGE_LOAD,
     [](const auto& i) { return IsVgpr(i.dst, 3) && IsVgpr(i.src0, 24) && i.dmask == 0xfu; }},
    {H::kPcColorDesc, Opcode::S_LOAD_DWORDX8, [](const auto& i) { return DescLoad(i, 0, 448); }},
    {H::kPcColorStore, Opcode::IMAGE_STORE,
     [](const auto& i) { return IsVgpr(i.dst, 3) && IsVgpr(i.src0, 24) && i.dmask == 0xfu; }},
    {H::kPcLumaDesc, Opcode::S_LOAD_DWORDX8, [](const auto& i) { return DescLoad(i, 0, 480); }},
    {H::kPcLumaStore, Opcode::IMAGE_STORE,
     [](const auto& i) { return IsVgpr(i.dst, 8) && IsVgpr(i.src0, 24) && i.dmask == 0x1u; }},
    {H::kPcEnd, Opcode::S_ENDPGM, [](const auto&) { return true; }},
}};

IR::MemoryInfo LdsWord() {
	IR::MemoryInfo memory;
	memory.kind            = IR::ResourceKind::Lds;
	memory.data_dwords     = 1;
	memory.data_bits       = 32;
	memory.component_count = 1;
	return memory;
}

} // namespace

std::optional<HalfRatePlan> MakeHalfRatePlan(const Decoder::Program& decoded, uint32_t lds_dwords,
                                             std::string* reason) {
	for (const auto& expect: kExpected) {
		const auto* inst = FindPc(decoded, expect.pc);
		if (inst == nullptr || inst->opcode != expect.opcode || !expect.shape(*inst)) {
			if (reason != nullptr) {
				*reason = "unexpected instruction at pc 0x" + std::to_string(expect.pc);
			}
			return std::nullopt;
		}
	}
	if (lds_dwords < H::kLdsDwords) {
		if (reason != nullptr) {
			*reason = "LDS size does not include the patch";
		}
		return std::nullopt;
	}
	HalfRatePlan plan;
	plan.lds_base_bytes = (lds_dwords - H::kLdsDwords) * 4u;
	plan.depth_desc     = *FindPc(decoded, H::kPcDepthDesc);
	plan.gbuffer_desc   = *FindPc(decoded, H::kPcGbufferDesc);
	plan.depth_load     = *FindPc(decoded, H::kPcDepthLoad);
	plan.gbuffer_load   = *FindPc(decoded, H::kPcGbufferLoad);
	plan.color_desc     = *FindPc(decoded, H::kPcColorDesc);
	plan.color_store    = *FindPc(decoded, H::kPcColorStore);
	plan.luma_desc      = *FindPc(decoded, H::kPcLumaDesc);
	plan.luma_store     = *FindPc(decoded, H::kPcLumaStore);
	return plan;
}

IR::U32 Translator::HalfRateConst(uint32_t value) {
	return IR::U32(IR::Value(value));
}

IR::F32 Translator::HalfRateFloat(float value) {
	return IR::F32(IR::Value::F32(value));
}

IR::F32 Translator::HalfRateFloatOp(IR::ValueOpcode opcode, IR::F32 lhs, IR::F32 rhs) {
	return IR::F32(ir.Emit(opcode, {lhs, rhs}));
}

IR::U32 Translator::HalfRateFlatIndex() {
	const auto local = [&](uint32_t axis) {
		return IR::U32(ir.Emit(IR::ValueOpcode::GetBuiltin,
		                       {IR::Value(static_cast<uint32_t>(IR::StageInputKind::LocalInvocationId)),
		                        IR::Value(axis)}));
	};
	return ir.IAdd(ir.ShiftLeftLogical(local(1), HalfRateConst(4)), local(0));
}

IR::U32 Translator::HalfRateLdsAddress(uint32_t array, IR::U32 index) {
	return ir.IAdd(HalfRateConst(half_rate->lds_base_bytes + array * H::kSamples * 4u),
	               ir.ShiftLeftLogical(index, HalfRateConst(2)));
}

void Translator::HalfRateStash(uint32_t array, IR::U32 index, IR::U32 value) {
	WriteSharedU32(1, HalfRateLdsAddress(array, index), {value, IR::Value(0u), IR::Value(0u), IR::Value(0u)},
	               LdsWord(), current_pc);
}

IR::U32 Translator::HalfRateLoad(uint32_t array, IR::U32 index) {
	return IR::U32(LoadSharedU32(1, HalfRateLdsAddress(array, index), LdsWord(), current_pc));
}

void Translator::HalfRateBefore(const Decoder::Instruction& inst) {
	if (half_rate == nullptr) {
		return;
	}
	if (inst.pc == H::kPcBoundsCmp) {
		// s12/s13 now hold the screen extent the kernel checks its pixels against.
		ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentX), ir.GetScalarReg(static_cast<IR::ScalarReg>(12)));
		ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentY), ir.GetScalarReg(static_cast<IR::ScalarReg>(13)));
	} else if (inst.pc == H::kPcEnd) {
		HalfRateReconstruct();
	}
}

void Translator::HalfRateAfter(const Decoder::Instruction& inst) {
	if (half_rate == nullptr) {
		return;
	}
	switch (inst.pc) {
		case H::kPcCoordY: HalfRateRemap(); break;
		case H::kPcDepthLoad:
			HalfRateStash(Depth, HalfRateFlatIndex(), ir.GetVectorReg(static_cast<IR::VectorReg>(26)));
			break;
		case H::kPcGbufferLoad:
			for (uint32_t k = 0; k < 4u; k++) {
				HalfRateStash(Gbuffer + k, HalfRateFlatIndex(), ir.GetVectorReg(static_cast<IR::VectorReg>(3u + k)));
			}
			break;
		case H::kPcColorStore:
			for (uint32_t k = 0; k < 4u; k++) {
				HalfRateStash(Color + k, HalfRateFlatIndex(), ir.GetVectorReg(static_cast<IR::VectorReg>(3u + k)));
			}
			HalfRateStash(Valid, HalfRateFlatIndex(), HalfRateConst(1));
			break;
		case H::kPcLumaStore:
			HalfRateStash(Luma, HalfRateFlatIndex(), ir.GetVectorReg(static_cast<IR::VectorReg>(8)));
			break;
		default: break;
	}
}

// After the prologue: wave 0 takes the even/even pixels of the tile, the other waves get
// coordinates that fail the kernel's bounds check. Wave 0 clears its sample's valid flag.
void Translator::HalfRateRemap() {
	const auto origin_x = ir.GetScalarReg(static_cast<IR::ScalarReg>(12));
	const auto origin_y = ir.GetScalarReg(static_cast<IR::ScalarReg>(13));
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX), origin_x);
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY), origin_y);
	const auto flat    = HalfRateFlatIndex();
	const auto wave0   = ir.ULessThan(flat, HalfRateConst(H::kSamples));
	const auto sample_x = ir.IAdd(origin_x, ir.ShiftLeftLogical(ir.BitwiseAnd(flat, HalfRateConst(7)), HalfRateConst(1)));
	const auto sample_y = ir.IAdd(origin_y, ir.ShiftLeftLogical(ir.ShiftRightLogical(flat, HalfRateConst(3)), HalfRateConst(1)));
	const auto outside  = HalfRateConst(0xffffffffu);
	ir.SetVectorReg(static_cast<IR::VectorReg>(24), ir.Select(wave0, sample_x, outside));
	ir.SetVectorReg(static_cast<IR::VectorReg>(25), ir.Select(wave0, sample_y, outside));
	const auto exec = ir.GetExec();
	ir.SetExec(ir.LogicalAnd(exec, wave0));
	HalfRateStash(Valid, flat, HalfRateConst(0));
	ir.SetExec(exec);
}

void Translator::HalfRateReconstruct() {
	using O = IR::ValueOpcode;
	ir.Emit(O::Barrier);
	const auto flat = HalfRateFlatIndex();
	const auto lx   = ir.BitwiseAnd(flat, HalfRateConst(kTileSize - 1u));
	const auto ly   = ir.ShiftRightLogical(flat, HalfRateConst(4));
	const auto px   = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX)), lx);
	const auto py   = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY)), ly);
	const auto odd  = ir.INotEqual(ir.BitwiseAnd(ir.BitwiseOr(lx, ly), HalfRateConst(1)), HalfRateConst(0));
	const auto inside =
	    ir.LogicalAnd(ir.ULessThan(px, ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentX))),
	                  ir.ULessThan(py, ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentY))));
	ir.SetExec(ir.LogicalAnd(odd, inside));
	ir.SetVectorReg(static_cast<IR::VectorReg>(24), px);
	ir.SetVectorReg(static_cast<IR::VectorReg>(25), py);
	TranslateInstruction(half_rate->depth_desc);
	TranslateInstruction(half_rate->depth_load);
	TranslateInstruction(half_rate->gbuffer_desc);
	TranslateInstruction(half_rate->gbuffer_load);

	const auto f   = [&](IR::U32 bits) { return ir.BitCastF32(bits); };
	const auto add = [&](IR::F32 a, IR::F32 b) { return HalfRateFloatOp(O::FPAdd32, a, b); };
	const auto sub = [&](IR::F32 a, IR::F32 b) { return HalfRateFloatOp(O::FPSub32, a, b); };
	const auto mul = [&](IR::F32 a, IR::F32 b) { return HalfRateFloatOp(O::FPMul32, a, b); };
	const auto rcp = [&](IR::F32 a) { return IR::F32(ir.Emit(O::FPRecip32, {a})); };
	const auto one = HalfRateFloat(1.0f);

	const auto own_linear = sub(one, f(ir.GetVectorReg(static_cast<IR::VectorReg>(26))));
	const auto own_scale  = rcp(HalfRateFloatOp(O::FPMax32, IR::F32(ir.Emit(O::FPAbs32, {own_linear})), HalfRateFloat(1e-7f)));
	std::array<IR::F32, 4> own_g {};
	auto                   own_g_energy = HalfRateFloat(0.0f);
	for (uint32_t k = 0; k < 4u; k++) {
		own_g[k]     = f(ir.GetVectorReg(static_cast<IR::VectorReg>(3u + k)));
		own_g_energy = add(own_g_energy, mul(own_g[k], own_g[k]));
	}

	// The 1 or 2 even sample columns/rows around the pixel, inside the tile.
	const auto x0 = ir.BitwiseAnd(lx, HalfRateConst(~1u));
	const auto y0 = ir.BitwiseAnd(ly, HalfRateConst(~1u));
	const auto x1 = IR::U32(ir.Emit(O::UMin32, {ir.IAdd(x0, ir.ShiftLeftLogical(ir.BitwiseAnd(lx, HalfRateConst(1)), HalfRateConst(1))), HalfRateConst(kLastSample)}));
	const auto y1 = IR::U32(ir.Emit(O::UMin32, {ir.IAdd(y0, ir.ShiftLeftLogical(ir.BitwiseAnd(ly, HalfRateConst(1)), HalfRateConst(1))), HalfRateConst(kLastSample)}));

	std::array<IR::F32, 4> color {};
	color.fill(HalfRateFloat(0.0f));
	auto luma   = HalfRateFloat(0.0f);
	auto weight = HalfRateFloat(0.0f);
	for (const auto& [sx, sy]: {std::pair {x0, y0}, std::pair {x1, y0}, std::pair {x0, y1}, std::pair {x1, y1}}) {
		const auto index = ir.IAdd(ir.ShiftRightLogical(sx, HalfRateConst(1)),
		                           ir.ShiftLeftLogical(ir.ShiftRightLogical(sy, HalfRateConst(1)), HalfRateConst(3)));
		const auto valid  = ir.IEqual(HalfRateLoad(Valid, index), HalfRateConst(1));
		const auto linear = sub(one, f(HalfRateLoad(Depth, index)));
		const auto depth_t =
		    mul(mul(IR::F32(ir.Emit(O::FPAbs32, {sub(linear, own_linear)})), own_scale), HalfRateFloat(kDepthSharpness));
		const auto depth_w = rcp(add(one, mul(depth_t, depth_t)));
		auto       g_diff   = HalfRateFloat(0.0f);
		auto       g_energy = own_g_energy;
		for (uint32_t k = 0; k < 4u; k++) {
			const auto g = f(HalfRateLoad(Gbuffer + k, index));
			const auto d = sub(g, own_g[k]);
			g_diff       = add(g_diff, mul(d, d));
			g_energy     = add(g_energy, mul(g, g));
		}
		const auto g_t = mul(mul(g_diff, rcp(add(g_energy, HalfRateFloat(1e-3f)))), HalfRateFloat(kGbufferSharpness));
		const auto g_w = rcp(add(one, g_t));
		const auto w   = IR::F32(ir.Emit(O::SelectF32, {valid, add(mul(depth_w, g_w), HalfRateFloat(kWeightFloor)), HalfRateFloat(0.0f)}));
		for (uint32_t k = 0; k < 4u; k++) {
			color[k] = IR::F32(ir.Emit(O::FPFma32, {w, f(HalfRateLoad(Color + k, index)), color[k]}));
		}
		luma   = IR::F32(ir.Emit(O::FPFma32, {w, f(HalfRateLoad(Luma, index)), luma}));
		weight = add(weight, w);
	}
	const auto any   = IR::U1(ir.Emit(O::FPOrdGreaterThan32, {weight, HalfRateFloat(0.0f)}));
	const auto scale = IR::F32(ir.Emit(O::SelectF32, {any, rcp(HalfRateFloatOp(O::FPMax32, weight, HalfRateFloat(1e-30f))), HalfRateFloat(0.0f)}));
	for (uint32_t k = 0; k < 4u; k++) {
		ir.SetVectorReg(static_cast<IR::VectorReg>(3u + k), ir.BitCastU32(mul(color[k], scale)));
	}
	ir.SetVectorReg(static_cast<IR::VectorReg>(8), ir.BitCastU32(mul(luma, scale)));
	TranslateInstruction(half_rate->color_desc);
	TranslateInstruction(half_rate->color_store);
	TranslateInstruction(half_rate->luma_desc);
	TranslateInstruction(half_rate->luma_store);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
