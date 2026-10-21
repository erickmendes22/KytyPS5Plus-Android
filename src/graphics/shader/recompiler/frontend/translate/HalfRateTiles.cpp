#include "graphics/shader/recompiler/frontend/translate/HalfRateTiles.h"

#include "graphics/shader/halfRateTiles.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

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

// Joint bilateral weights, each 1 / (1 + t^2): spatial (sample distance in pixels), relative
// linear-depth difference (standard Z, far = 1, so 1 - depth is proportional to 1 / view depth)
// and normalized G-buffer difference. Steep enough that another surface's sample only matters
// when no same-surface sample is in reach.
constexpr float    kSpatialScale     = 0.7f;
constexpr float    kDepthSharpness   = 32.0f;
constexpr float    kGbufferSharpness = 16.0f;
constexpr uint32_t kTileSize         = H::kGuestGroupSize;
constexpr uint32_t kLastSample       = kTileSize - 2u;

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

bool DescLoad(const Decoder::Instruction& inst, uint32_t dst, uint32_t offset) {
	return IsSgpr(inst.dst, dst) && inst.offset == offset && IsSgpr(inst.src0, 14);
}

struct Expect {
	uint32_t pc;
	Opcode   opcode;
	bool (*shape)(const Decoder::Instruction&);
};

bool Any(const Decoder::Instruction&) { return true; }

const std::array kExpected {
    Expect {H::kPcFlatIndex, Opcode::V_LSHL_ADD_U32, [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 23); }},
    Expect {0x390, Opcode::V_CMPX_GT_U32, Any},
    Expect {0x3b4, Opcode::V_CMPX_LE_U32, Any},
    Expect {0x3bc, Opcode::V_MUL_LO_U32, Any},
    Expect {0x4a0, Opcode::S_MOV_B64, Any},
    Expect {0x4b4, Opcode::DS_ADD_RTN_U32, [](const Decoder::Instruction& i) { return i.offset == H::kListCounterByte; }},
    Expect {0x4cc, Opcode::DS_WRITE_B32, [](const Decoder::Instruction& i) { return i.offset == H::kListEntriesByte; }},
    Expect {H::kPcCullEnd, Opcode::S_MOV_B64, Any},
    Expect {H::kPcCoordX, Opcode::V_ADD_NC_U32, [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 24); }},
    Expect {H::kPcCoordY, Opcode::V_ADD_NC_U32, [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 25); }},
    Expect {H::kPcListBarrier, Opcode::S_BARRIER, Any},
    Expect {H::kPcBoundsCmp, Opcode::V_CMP_LE_U32, Any},
    Expect {H::kPcDepthDesc, Opcode::S_LOAD_DWORDX8, [](const Decoder::Instruction& i) { return DescLoad(i, 16, 128); }},
    Expect {H::kPcGbufferDesc, Opcode::S_LOAD_DWORDX8, [](const Decoder::Instruction& i) { return DescLoad(i, 24, 0); }},
    Expect {H::kPcDepthLoad, Opcode::IMAGE_LOAD,
            [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 26) && IsVgpr(i.src0, 24) && i.dmask == 0x1u; }},
    Expect {H::kPcGbufferLoad, Opcode::IMAGE_LOAD,
            [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 3) && IsVgpr(i.src0, 24) && i.dmask == 0xfu; }},
    Expect {H::kPcColorDesc, Opcode::S_LOAD_DWORDX8, [](const Decoder::Instruction& i) { return DescLoad(i, 0, 448); }},
    Expect {H::kPcColorStore, Opcode::IMAGE_STORE,
            [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 3) && IsVgpr(i.src0, 24) && i.dmask == 0xfu; }},
    Expect {H::kPcLumaDesc, Opcode::S_LOAD_DWORDX8, [](const Decoder::Instruction& i) { return DescLoad(i, 0, 480); }},
    Expect {H::kPcLumaStore, Opcode::IMAGE_STORE,
            [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 8) && IsVgpr(i.src0, 24) && i.dmask == 0x1u; }},
    Expect {H::kPcEnd, Opcode::S_ENDPGM, Any},
};

// Registers an instruction writes from its destination: loads write data_dwords, ALU ops one.
bool WritesVgpr(const Decoder::Instruction& inst, const Decoder::Operand& operand, uint32_t reg) {
	if (operand.kind != OperandKind::Vgpr) {
		return false;
	}
	const bool load = inst.family == Decoder::Family::DS || inst.family == Decoder::Family::MUBUF ||
	                  inst.family == Decoder::Family::MTBUF || inst.family == Decoder::Family::MIMG ||
	                  inst.family == Decoder::Family::FLAT;
	const uint32_t width = load ? std::max(inst.data_dwords, 1u) : 1u;
	return operand.reg <= reg && reg < operand.reg + width;
}

// The item code must be straight-line apart from EXECZ skips inside the range, must not write
// v23 (the item index the replay substitutes), and its only LDS writes are the list append.
bool CheckCullRange(const Decoder::Program& decoded, std::vector<Decoder::Instruction>& out, std::string* reason) {
	const auto fail = [&](const Decoder::Instruction& inst, const char* why) {
		if (reason != nullptr) {
			*reason = std::string(why) + " at pc 0x" + std::to_string(inst.pc) + ": " + Decoder::InstructionToString(inst);
		}
		return false;
	};
	for (const auto& inst: decoded.instructions) {
		if (inst.pc < H::kPcCullBegin || inst.pc >= H::kPcCullEnd) {
			continue;
		}
		const std::string text = Decoder::InstructionToString(inst);
		const std::string_view name(text);
		if (inst.opcode == Opcode::S_CBRANCH_EXECZ) {
			if (inst.branch_target <= inst.pc || inst.branch_target > H::kPcCullEnd) {
				return fail(inst, "EXECZ skip leaves the item code");
			}
			continue; // EXEC predication alone gives the same result
		}
		if (name.starts_with("S_BRANCH") || name.starts_with("S_CBRANCH") || name.starts_with("S_SETPC") ||
		    name.starts_with("S_SWAPPC") || name.starts_with("S_ENDPGM") || inst.opcode == Opcode::S_BARRIER) {
			return fail(inst, "control flow in the item code");
		}
		if (WritesVgpr(inst, inst.dst, 23) || WritesVgpr(inst, inst.dst2, 23)) {
			return fail(inst, "item code writes v23");
		}
		const bool lds_write = inst.family == Decoder::Family::DS && name.find("READ") == std::string_view::npos;
		if (lds_write && inst.pc != 0x4b4 && inst.pc != 0x4cc) {
			return fail(inst, "unexpected LDS write in the item code");
		}
		out.push_back(inst);
	}
	return true;
}

IR::MemoryInfo LdsWord() {
	IR::MemoryInfo memory;
	memory.kind            = IR::ResourceKind::Lds;
	memory.data_dwords     = 1;
	memory.data_bits       = 32;
	memory.component_count = 1;
	return memory;
}

} // namespace

std::optional<HalfRatePlan> MakeHalfRatePlan(const Decoder::Program& decoded, uint8_t variant,
                                             uint32_t lds_dwords, uint8_t fill_mode, bool capture,
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
	HalfRatePlan plan;
	plan.half           = variant == H::kVariantHalf;
	plan.capture        = capture;
	plan.capture_region = plan.half ? H::kCheckRegionHalf : H::kCheckRegionNative;
	plan.fill_mode      = fill_mode;
	if (!plan.half) {
		return plan;
	}
	if (lds_dwords < H::kLdsDwords) {
		if (reason != nullptr) {
			*reason = "LDS size does not include the patch";
		}
		return std::nullopt;
	}
	if (!CheckCullRange(decoded, plan.cull, reason)) {
		return std::nullopt;
	}
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

IR::U32 Translator::HalfRateLocal(uint32_t axis) {
	return IR::U32(ir.Emit(IR::ValueOpcode::GetBuiltin,
	                       {IR::Value(static_cast<uint32_t>(IR::StageInputKind::LocalInvocationId)), IR::Value(axis)}));
}

// Thread index in the 8x8 group (= sample index of its even/even pixel).
IR::U32 Translator::HalfRateFlatIndex() {
	return ir.IAdd(ir.ShiftLeftLogical(HalfRateLocal(1), HalfRateConst(3)), HalfRateLocal(0));
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
	if (inst.pc == 0u && half_rate->half) {
		ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegTileLive), HalfRateConst(0));
	}
	if (!half_rate->half) {
		return;
	}
	switch (inst.pc) {
		case H::kPcCullBegin: HalfRateCull(); break;
		case H::kPcBoundsCmp:
			// s12/s13 now hold the screen extent the kernel checks its pixels against.
			ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentX), ir.GetScalarReg(static_cast<IR::ScalarReg>(12)));
			ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentY), ir.GetScalarReg(static_cast<IR::ScalarReg>(13)));
			break;
		case H::kPcEnd: HalfRateReconstruct(); break;
		default: break;
	}
}

void Translator::HalfRateAfter(const Decoder::Instruction& inst) {
	if (half_rate == nullptr) {
		return;
	}
	if (inst.pc == H::kPcListBarrier && half_rate->capture) {
		HalfRateCapture();
	}
	if (!half_rate->half) {
		return;
	}
	switch (inst.pc) {
		case H::kPcFlatIndex:
			// The item index of the guest's first culling pass; the replay adds 64k.
			ir.SetVectorReg(static_cast<IR::VectorReg>(23), HalfRateFlatIndex());
			break;
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

// Light culling for 256 items with 64 threads: the guest's item code, 4 passes with
// v23 = thread + 64k, each from the EXEC the guest has there. The original code then runs with
// EXEC off and v3 = 0, so it skips to the end of the culling without appending.
void Translator::HalfRateCull() {
	const auto exec   = ir.GetExec();
	const auto thread = HalfRateFlatIndex();
	for (uint32_t pass = 0; pass < H::kCullPasses; pass++) {
		ir.SetExec(exec);
		ir.SetVectorReg(static_cast<IR::VectorReg>(23), ir.IAdd(thread, HalfRateConst(pass * H::kThreads)));
		for (const auto& inst: half_rate->cull) {
			TranslateInstruction(inst);
		}
	}
	ir.SetVectorReg(static_cast<IR::VectorReg>(23), thread);
	ir.SetVectorReg(static_cast<IR::VectorReg>(3), HalfRateConst(0));
	ir.SetExec(IR::U1(IR::Value(false)));
}

// After the prologue: each thread takes the even/even pixel of its 2x2 and clears its sample's
// valid flag.
void Translator::HalfRateRemap() {
	const auto origin_x = ir.GetScalarReg(static_cast<IR::ScalarReg>(12));
	const auto origin_y = ir.GetScalarReg(static_cast<IR::ScalarReg>(13));
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX), origin_x);
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY), origin_y);
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegTileLive), HalfRateConst(1));
	// kFillQuarter (timing probe): contiguous pixels of the top-left quarter instead of every other one.
	const uint32_t stride_shift = half_rate->fill_mode == H::kFillQuarter ? 0u : 1u;
	ir.SetVectorReg(static_cast<IR::VectorReg>(24), ir.IAdd(origin_x, ir.ShiftLeftLogical(HalfRateLocal(0), HalfRateConst(stride_shift))));
	ir.SetVectorReg(static_cast<IR::VectorReg>(25), ir.IAdd(origin_y, ir.ShiftLeftLogical(HalfRateLocal(1), HalfRateConst(stride_shift))));
	HalfRateStash(Valid, HalfRateFlatIndex(), HalfRateConst(0));
}

// KYTY_RT_HALF_CHECK: thread 0 writes the tile's light list (count + 1, 16 entries).
void Translator::HalfRateCapture() {
	const auto exec   = ir.GetExec();
	const auto lead   = ir.LogicalAnd(ir.IEqual(HalfRateLocal(0), HalfRateConst(0)),
	                                  ir.IEqual(HalfRateLocal(1), HalfRateConst(0)));
	const auto tile_x = ir.ShiftRightLogical(ir.GetScalarReg(static_cast<IR::ScalarReg>(12)), HalfRateConst(4));
	const auto tile_y = ir.ShiftRightLogical(ir.GetScalarReg(static_cast<IR::ScalarReg>(13)), HalfRateConst(4));
	const auto inside = ir.LogicalAnd(ir.ULessThan(tile_x, HalfRateConst(H::kCheckTilesX)),
	                                  ir.ULessThan(tile_y, HalfRateConst(H::kCheckTilesY)));
	const auto write  = ir.LogicalAnd(lead, inside);
	ir.SetExec(write);
	const auto tile = ir.IAdd(ir.IMul(tile_y, HalfRateConst(H::kCheckTilesX)), tile_x);
	const auto base = ir.IAdd(HalfRateConst(half_rate->capture_region * H::kCheckRegionDwords),
	                          ir.IMul(tile, HalfRateConst(H::kCheckEntryDwords)));
	const auto read = [&](uint32_t byte) {
		return IR::U32(LoadSharedU32(1, HalfRateConst(byte), LdsWord(), current_pc));
	};
	const auto log = [&](uint32_t word, IR::U32 value) {
		ir.Emit(IR::ValueOpcode::DebugLogStoreU32, {ir.IAdd(base, HalfRateConst(word)), value, write});
	};
	log(0, ir.IAdd(read(H::kListCounterByte), HalfRateConst(1)));
	for (uint32_t i = 0; i < H::kListEntries; i++) {
		log(1 + i, read(H::kListEntriesByte + i * 4u));
	}
	ir.SetExec(exec);
}

// One pixel of the 2x2 that is not shaded, at (2x + dx, 2y + dy) for thread (x, y). Candidates
// are samples at (2x + ox, 2y + oy): kFillNearest4 the 1, 2 or 4 nearest (ox, oy in {0, 2 dx/dy}),
// kFillRing9 the 3x3 grid ox, oy in {-2, 0, 2}. Weights: distance, depth and G-buffer similarity.
void Translator::HalfRateFill(uint32_t dx_pixel, uint32_t dy_pixel, IR::U1 live) {
	using O = IR::ValueOpcode;
	const auto bx = ir.ShiftLeftLogical(HalfRateLocal(0), HalfRateConst(1));
	const auto by = ir.ShiftLeftLogical(HalfRateLocal(1), HalfRateConst(1));
	const auto tx = ir.IAdd(bx, HalfRateConst(dx_pixel));
	const auto ty = ir.IAdd(by, HalfRateConst(dy_pixel));
	const auto px = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX)), tx);
	const auto py = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY)), ty);
	const auto inside =
	    ir.LogicalAnd(ir.ULessThan(px, ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentX))),
	                  ir.ULessThan(py, ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegExtentY))));
	ir.SetExec(ir.LogicalAnd(live, inside));
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
	const auto abs = [&](IR::F32 a) { return IR::F32(ir.Emit(O::FPAbs32, {a})); };
	const auto one = HalfRateFloat(1.0f);
	const auto falloff = [&](IR::F32 t) { return rcp(add(one, mul(t, t))); };

	const auto own_linear = sub(one, f(ir.GetVectorReg(static_cast<IR::VectorReg>(26))));
	const auto own_scale  = rcp(HalfRateFloatOp(O::FPMax32, abs(own_linear), HalfRateFloat(1e-7f)));
	std::array<IR::F32, 4> own_g {};
	auto                   own_g_energy = HalfRateFloat(0.0f);
	for (uint32_t k = 0; k < 4u; k++) {
		own_g[k]     = f(ir.GetVectorReg(static_cast<IR::VectorReg>(3u + k)));
		own_g_energy = add(own_g_energy, mul(own_g[k], own_g[k]));
	}

	const auto ftx = IR::F32(ir.Emit(O::ConvertF32U32, {tx}));
	const auto fty = IR::F32(ir.Emit(O::ConvertF32U32, {ty}));

	std::array<IR::F32, 4> color {};
	color.fill(HalfRateFloat(0.0f));
	auto luma   = HalfRateFloat(0.0f);
	auto weight = HalfRateFloat(0.0f);
	std::vector<uint32_t> xs;
	std::vector<uint32_t> ys;
	if (half_rate->fill_mode == H::kFillRing9) {
		xs = {0xfffffffeu, 0u, 2u};
		ys = {0xfffffffeu, 0u, 2u};
	} else {
		xs = dx_pixel != 0u ? std::vector<uint32_t> {0u, 2u} : std::vector<uint32_t> {0u};
		ys = dy_pixel != 0u ? std::vector<uint32_t> {0u, 2u} : std::vector<uint32_t> {0u};
	}
	for (const uint32_t oy: ys) {
		for (const uint32_t ox: xs) {
			const auto cx      = ir.IAdd(bx, HalfRateConst(ox));
			const auto cy      = ir.IAdd(by, HalfRateConst(oy));
			const auto in_tile = ir.LogicalAnd(ir.ULessThan(cx, HalfRateConst(kLastSample + 1u)),
			                                   ir.ULessThan(cy, HalfRateConst(kLastSample + 1u)));
			const auto safe_x  = IR::U32(ir.Emit(O::UMin32, {cx, HalfRateConst(kLastSample)}));
			const auto safe_y  = IR::U32(ir.Emit(O::UMin32, {cy, HalfRateConst(kLastSample)}));
			const auto index   = ir.IAdd(ir.ShiftRightLogical(safe_x, HalfRateConst(1)),
			                             ir.ShiftLeftLogical(ir.ShiftRightLogical(safe_y, HalfRateConst(1)), HalfRateConst(3)));
			const auto valid   = ir.LogicalAnd(in_tile, ir.IEqual(HalfRateLoad(Valid, index), HalfRateConst(1)));
			const auto dx      = sub(ftx, IR::F32(ir.Emit(O::ConvertF32U32, {safe_x})));
			const auto dy      = sub(fty, IR::F32(ir.Emit(O::ConvertF32U32, {safe_y})));
			const auto spatial = rcp(add(one, mul(add(mul(dx, dx), mul(dy, dy)), HalfRateFloat(kSpatialScale))));
			const auto linear  = sub(one, f(HalfRateLoad(Depth, index)));
			const auto depth_w = falloff(mul(mul(abs(sub(linear, own_linear)), own_scale), HalfRateFloat(kDepthSharpness)));
			auto       g_diff   = HalfRateFloat(0.0f);
			auto       g_energy = own_g_energy;
			for (uint32_t k = 0; k < 4u; k++) {
				const auto g = f(HalfRateLoad(Gbuffer + k, index));
				const auto d = sub(g, own_g[k]);
				g_diff       = add(g_diff, mul(d, d));
				g_energy     = add(g_energy, mul(g, g));
			}
			const auto g_w = falloff(mul(mul(g_diff, rcp(add(g_energy, HalfRateFloat(1e-4f)))), HalfRateFloat(kGbufferSharpness)));
			const auto w   = IR::F32(ir.Emit(O::SelectF32, {valid, mul(mul(spatial, depth_w), g_w), HalfRateFloat(0.0f)}));
			for (uint32_t k = 0; k < 4u; k++) {
				color[k] = IR::F32(ir.Emit(O::FPFma32, {w, f(HalfRateLoad(Color + k, index)), color[k]}));
			}
			luma   = IR::F32(ir.Emit(O::FPFma32, {w, f(HalfRateLoad(Luma, index)), luma}));
			weight = add(weight, w);
		}
	}
	const auto any   = IR::U1(ir.Emit(O::FPOrdGreaterThan32, {weight, HalfRateFloat(0.0f)}));
	const auto scale = IR::F32(ir.Emit(O::SelectF32, {any, rcp(HalfRateFloatOp(O::FPMax32, weight, HalfRateFloat(1e-37f))), HalfRateFloat(0.0f)}));
	for (uint32_t k = 0; k < 4u; k++) {
		ir.SetVectorReg(static_cast<IR::VectorReg>(3u + k), ir.BitCastU32(mul(color[k], scale)));
	}
	ir.SetVectorReg(static_cast<IR::VectorReg>(8), ir.BitCastU32(mul(luma, scale)));
	TranslateInstruction(half_rate->color_desc);
	TranslateInstruction(half_rate->color_store);
	TranslateInstruction(half_rate->luma_desc);
	TranslateInstruction(half_rate->luma_store);
}

// Before s_endpgm: all samples of the tile are in LDS after the barrier; each thread fills the
// right, lower and diagonal pixel of its 2x2.
void Translator::HalfRateReconstruct() {
	if (half_rate->fill_mode == H::kFillOff || half_rate->fill_mode == H::kFillQuarter) {
		return;
	}
	ir.Emit(IR::ValueOpcode::Barrier);
	const auto live = ir.IEqual(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegTileLive)), HalfRateConst(1));
	HalfRateFill(1, 0, live);
	HalfRateFill(0, 1, live);
	HalfRateFill(1, 1, live);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
