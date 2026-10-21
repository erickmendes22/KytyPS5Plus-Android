#include "graphics/shader/recompiler/frontend/translate/HalfRateTiles.h"

#include "graphics/shader/halfRateTiles.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <cstdlib>
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
    Expect {H::kPcGroupId, Opcode::S_LSHL_B32, [](const Decoder::Instruction& i) { return IsSgpr(i.src0, 16); }},
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
    Expect {H::kPcMaterialDesc, Opcode::S_LOAD_DWORDX8, [](const Decoder::Instruction& i) { return DescLoad(i, 0, 224); }},
    Expect {H::kPcMaterialLoad, Opcode::IMAGE_LOAD,
            [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 0) && IsVgpr(i.src0, 24) && i.dmask == 0x1u; }},
    Expect {H::kPcAlbedoDesc, Opcode::S_LOAD_DWORDX8, [](const Decoder::Instruction& i) { return DescLoad(i, 20, 32); }},
    Expect {H::kPcAlbedoLoad, Opcode::IMAGE_LOAD,
            [](const Decoder::Instruction& i) { return IsVgpr(i.dst, 6) && IsVgpr(i.src0, 24) && i.dmask == 0xfu; }},
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
	const bool variant3 = variant == H::kVariantAdaptive;
	plan.adaptive     = variant3 && H::LoopDiagnostic();
	plan.retrace      = variant3 && !plan.adaptive;
	plan.retrace_pass = plan.retrace && fill_mode == H::kFillRetracePass;
	if (plan.retrace_pass) plan.fill_mode = 0;
	plan.half     = plan.half || variant3;
	plan.capture  = plan.capture && !variant3; // the check's stress split would share the loop's pre block
	plan.capture_region = plan.half ? H::kCheckRegionHalf : H::kCheckRegionNative;
	const uint32_t check_dwords    = H::CheckEnabled() && !variant3 ? H::kCheckLdsDwords : 0u;
	const uint32_t adaptive_dwords = variant3 ? H::kAdaptiveLdsDwords : 0u;
	const uint32_t patch_dwords    = (plan.half ? H::kLdsDwords : 0u) + adaptive_dwords + check_dwords;
	if (lds_dwords < patch_dwords) {
		if (reason != nullptr) {
			*reason = "LDS size does not include the patch";
		}
		return std::nullopt;
	}
	if (!CheckCullRange(decoded, plan.cull, reason)) {
		return std::nullopt;
	}
	plan.check_base_bytes    = (lds_dwords - check_dwords) * 4u;
	plan.adaptive_base_bytes = (lds_dwords - check_dwords - adaptive_dwords) * 4u;
	plan.lds_base_bytes      = (lds_dwords - patch_dwords) * 4u;
	if (plan.half) {
		plan.splits[plan.split_count++].pc = H::kPcCullBegin;
		if (plan.capture) {
			plan.splits[plan.split_count++].pc = H::kPcListBarrier;
		}
	}
	if (!plan.half) {
		return plan;
	}
	plan.depth_desc     = *FindPc(decoded, H::kPcDepthDesc);
	plan.gbuffer_desc   = *FindPc(decoded, H::kPcGbufferDesc);
	plan.depth_load     = *FindPc(decoded, H::kPcDepthLoad);
	plan.gbuffer_load   = *FindPc(decoded, H::kPcGbufferLoad);
	plan.color_desc     = *FindPc(decoded, H::kPcColorDesc);
	plan.color_store    = *FindPc(decoded, H::kPcColorStore);
	plan.luma_desc      = *FindPc(decoded, H::kPcLumaDesc);
	plan.luma_store     = *FindPc(decoded, H::kPcLumaStore);
	plan.material_desc  = *FindPc(decoded, H::kPcMaterialDesc);
	plan.material_load  = *FindPc(decoded, H::kPcMaterialLoad);
	plan.albedo_desc    = *FindPc(decoded, H::kPcAlbedoDesc);
	plan.albedo_load    = *FindPc(decoded, H::kPcAlbedoLoad);
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
	if (half_rate->adaptive && H::CheckEnabled() && inst.pc == 0x1d7cu) {
		ir.SetVectorReg(static_cast<IR::VectorReg>(kRegLightIterations), HalfRateConst(0)); // light loop entry
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
		case H::kPcEnd:
			if (half_rate->retrace_pass) {
				break; // pass 2 stored its pixels through the guest's own stores
			}
			if (half_rate->adaptive) {
				HalfRateLoopExit();
			} else {
				HalfRateReconstruct();
				if (half_rate->retrace) HalfRateExportList();
			}
			break;
		case H::kPcGroupId:
			if (half_rate->retrace_pass) HalfRateRetraceGroup();
			break;
		case H::kPcColorStore:
			// Diagnostic: KYTY_RT_HALF_RETRACE_RED=1 makes pass 2 store pure red.
			if (half_rate->retrace_pass && std::getenv("KYTY_RT_HALF_RETRACE_RED") != nullptr) {
				ir.SetVectorReg(static_cast<IR::VectorReg>(3), HalfRateConst(0x40800000u)); // 4.0f
				ir.SetVectorReg(static_cast<IR::VectorReg>(4), HalfRateConst(0));
				ir.SetVectorReg(static_cast<IR::VectorReg>(5), HalfRateConst(0));
				ir.SetVectorReg(static_cast<IR::VectorReg>(6), HalfRateConst(0x3f800000u));
			}
			break;
		default: break;
	}
}

void Translator::HalfRateAfter(const Decoder::Instruction& inst) {
	if (half_rate == nullptr) {
		return;
	}
	if (half_rate->adaptive && H::CheckEnabled() && (inst.pc == 0x1de4u || inst.pc == 0x1df0u)) {
		HalfRateLightLoopDump(inst.pc);
	}
		if (inst.pc == H::kPcListBarrier && half_rate->adaptive) {
		HalfRateLoopEnter();
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
		case H::kPcCoordY:
			if (half_rate->retrace_pass) {
				HalfRateRetraceRemap();
			} else {
				HalfRateRemap();
			}
			break;
		default: break;
	}
	if (half_rate->retrace_pass) {
		return; // pass 2 keeps no samples
	}
	switch (inst.pc) {
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
		case H::kPcMaterialLoad:
			if (half_rate->capture) HalfRateCheckStash(H::kCheckLdsMaterial, ir.GetVectorReg(static_cast<IR::VectorReg>(0)));
			break;
		case H::kPcAlbedoLoad:
			if (half_rate->capture) {
				for (uint32_t k = 0; k < 4u; k++) {
					HalfRateCheckStash(H::kCheckLdsAlbedo + k * H::kSamples, ir.GetVectorReg(static_cast<IR::VectorReg>(6u + k)));
				}
			}
			break;
		default: break;
	}
}

// Light culling for 256 items with 64 threads: the guest's item code, pass k with
// v23 = thread + 64k, each from the EXEC the guest has there. Passes 1..3 sit in their own blocks
// behind a uniform branch on 64k < item count (s51, loaded by pass 0's replay of pc 0x384): items
// at or past the count fail the guest's own first test, so a skipped pass is one that would append
// nothing. The original code then runs with EXEC off and v3 = 0, so it skips to the end of the
// culling without appending.
void Translator::HalfRateCull() {
	const auto exec   = ir.GetExec();
	const auto thread = HalfRateFlatIndex();
	const auto pass   = [&](uint32_t k) {
		ir.SetExec(exec);
		ir.SetVectorReg(static_cast<IR::VectorReg>(23), ir.IAdd(thread, HalfRateConst(k * H::kThreads)));
		for (const auto& inst: half_rate->cull) {
			TranslateInstruction(inst);
		}
	};
	pass(0);
	for (uint32_t k = 1; k < H::kCullPasses; k++) {
		auto*      from  = ir.CurrentBlock();
		const auto count = ir.GetScalarReg(static_cast<IR::ScalarReg>(51));
		const auto taken = ir.ULessThan(HalfRateConst(k * H::kThreads), count);
		from->condition  = taken;
		ir.Emit(IR::ValueOpcode::Reference, {taken});
		ir.SetBlock(half_rate->splits[0].pass[k - 1u]);
		pass(k);
		ir.SetBlock(half_rate->splits[0].tail[k - 1u]);
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
	const uint32_t stride_shift = half_rate->fill_mode == H::kFillQuarter && !half_rate->adaptive && !half_rate->retrace ? 0u : 1u;
	ir.SetVectorReg(static_cast<IR::VectorReg>(24), ir.IAdd(origin_x, ir.ShiftLeftLogical(HalfRateLocal(0), HalfRateConst(stride_shift))));
	ir.SetVectorReg(static_cast<IR::VectorReg>(25), ir.IAdd(origin_y, ir.ShiftLeftLogical(HalfRateLocal(1), HalfRateConst(stride_shift))));
	HalfRateStash(Valid, HalfRateFlatIndex(), HalfRateConst(0));
	if (half_rate->retrace) {
		// Pass 1: clear the LDS edge list (ordered before the fill by its barrier).
		const auto exec = ir.GetExec();
		ir.SetExec(ir.LogicalAnd(ir.IEqual(HalfRateLocal(0), HalfRateConst(0)), ir.IEqual(HalfRateLocal(1), HalfRateConst(0))));
		WriteSharedU32(1, HalfRateConst(half_rate->adaptive_base_bytes), {HalfRateConst(0), IR::Value(0u), IR::Value(0u), IR::Value(0u)},
		               LdsWord(), current_pc);
		ir.SetExec(exec);
	}
	if (half_rate->capture) {
		HalfRateCheckStash(H::kCheckLdsMaterial, HalfRateConst(0));
		for (uint32_t k = 0; k < 4u; k++) HalfRateCheckStash(H::kCheckLdsAlbedo + k * H::kSamples, HalfRateConst(0));
	}
}

// Check mode: one word per sample (thread) in the check scratch, array base `word` (in words).
void Translator::HalfRateCheckStash(uint32_t word, IR::U32 value) {
	const auto address = ir.IAdd(HalfRateConst(half_rate->check_base_bytes + word * 4u),
	                             ir.ShiftLeftLogical(HalfRateFlatIndex(), HalfRateConst(2)));
	WriteSharedU32(1, address, {value, IR::Value(0u), IR::Value(0u), IR::Value(0u)}, LdsWord(), current_pc);
}

IR::U32 Translator::HalfRateCheckLoad(uint32_t word, IR::U32 index) {
	const auto address = ir.IAdd(HalfRateConst(half_rate->check_base_bytes + word * 4u),
	                             ir.ShiftLeftLogical(index, HalfRateConst(2)));
	return IR::U32(LoadSharedU32(1, address, LdsWord(), current_pc));
}

// KYTY_RT_HALF_CHECK, after the list barrier (graphics/shader/halfRateTiles.h "Check log").
// 1) Synthetic stress culling: the guest's item code over the tile's designed item count, each
// record replaced by a zero position and a +-1e30 radius, appending through the guest's own atomic
// into an LDS scratch list (no 16-entry cap) and ORing a mask. Native: 256 threads x 1 item;
// half: 64 threads x 4 passes, passes 1..3 behind the same uniform branches as the real culling.
// 2) Thread 0 logs the real list, the item count and the stress results. Registers the item code
// writes are restored; the real list in LDS is not touched.
void Translator::HalfRateCapture() {
	using O = IR::ValueOpcode;
	const auto exec = ir.GetExec();
	std::array<IR::U32, 24> saved_v {};
	for (uint32_t r = 0; r < saved_v.size(); r++) saved_v[r] = ir.GetVectorReg(static_cast<IR::VectorReg>(3u + r));
	std::vector<std::pair<uint32_t, IR::U32>> saved_s;
	for (uint32_t r = 16; r <= 35; r++) saved_s.emplace_back(r, ir.GetScalarReg(static_cast<IR::ScalarReg>(r)));
	for (uint32_t r = 48; r <= 51; r++) saved_s.emplace_back(r, ir.GetScalarReg(static_cast<IR::ScalarReg>(r)));
	const auto saved_vcc_lo = ir.GetVccLo();
	const auto saved_vcc_hi = ir.GetVccHi();
	const auto saved_scc    = ir.GetScc();
	const auto item_count   = ir.GetScalarReg(static_cast<IR::ScalarReg>(51));

	const auto lx     = HalfRateLocal(0);
	const auto ly     = HalfRateLocal(1);
	const auto lead   = ir.LogicalAnd(ir.IEqual(lx, HalfRateConst(0)), ir.IEqual(ly, HalfRateConst(0)));
	const auto tile_x = ir.ShiftRightLogical(ir.GetScalarReg(static_cast<IR::ScalarReg>(12)), HalfRateConst(4));
	const auto tile_y = ir.ShiftRightLogical(ir.GetScalarReg(static_cast<IR::ScalarReg>(13)), HalfRateConst(4));
	const auto tile   = ir.IAdd(ir.IMul(tile_y, HalfRateConst(H::kCheckTilesX)), tile_x);
	// Designed count: kStressCounts[tile % 13], as a select chain on tile - 13 * (tile / 13).
	const auto tile_mod = ir.ISub(tile, ir.IMul(IR::U32(ir.Emit(O::ConvertU32F32, {IR::F32(ir.Emit(O::FPMul32,
	                          {IR::F32(ir.Emit(O::ConvertF32U32, {tile})), HalfRateFloat(1.0f / 13.0f)}))})),
	                          HalfRateConst(13)));
	auto designed = HalfRateConst(H::kStressCounts[0]);
	for (uint32_t i = 1; i < H::kStressCounts.size(); i++) {
		designed = ir.Select(ir.IEqual(tile_mod, HalfRateConst(i)), HalfRateConst(H::kStressCounts[i]), designed);
	}
	const auto check_word = [&](uint32_t word) { return HalfRateConst(half_rate->check_base_bytes + word * 4u); };
	ir.SetExec(lead);
	for (uint32_t w = 0; w < 1u + H::kCheckMaskDwords + H::kRejectCounters; w++) {
		const uint32_t word = w == 0 ? H::kCheckLdsCounter
		                    : w <= H::kCheckMaskDwords ? H::kCheckLdsMask + (w - 1u)
		                                               : H::kCheckLdsRejected + (w - 1u - H::kCheckMaskDwords);
		WriteSharedU32(1, check_word(word), {HalfRateConst(0), IR::Value(0u), IR::Value(0u), IR::Value(0u)}, LdsWord(),
		               current_pc);
	}
	ir.Emit(O::Barrier);

	IR::U32    start {};
	const auto one_e30     = HalfRateConst(0x7149f2cau); // +1e30f
	const auto minus_e30   = HalfRateConst(0xf149f2cau); // -1e30f
	const uint32_t per_axis = half_rate->half ? H::kGroupSize : H::kGuestGroupSize;
	const auto     thread   = ir.IAdd(ir.IMul(ly, HalfRateConst(per_axis)), lx);
	const auto     pass     = [&](uint32_t k) {
		const auto item = ir.IAdd(thread, HalfRateConst(k * per_axis * per_axis));
		ir.SetExec(exec);
		ir.SetVectorReg(static_cast<IR::VectorReg>(23), item);
		for (const auto& original: half_rate->cull) {
			auto inst = original;
			if (inst.pc == 0x4b4u) inst.offset = half_rate->check_base_bytes + H::kCheckLdsCounter * 4u;
			if (inst.pc == 0x4ccu) inst.offset = half_rate->check_base_bytes + H::kCheckLdsList * 4u;
			if (inst.pc == 0x4c0u) {
				inst.src0       = Decoder::Operand {};
				inst.src0.kind  = OperandKind::LiteralConstant;
				inst.src0.value = H::kForcedItems;
			}
			TranslateInstruction(inst);
			if (inst.pc == 0x384u) ir.SetScalarReg(static_cast<IR::ScalarReg>(51), designed);
			if (inst.pc == 0x3a8u && k == 0u) start = ir.GetVccLo();
			if (inst.pc == 0x408u) {
				// Zero position, radius +-1e30 by StressPasses(item, tile).
				const auto hash = ir.BitwiseAnd(ir.ShiftRightLogical(ir.BitwiseXor(ir.IMul(item, HalfRateConst(0x9e37u)),
				                                                                   ir.IMul(tile, HalfRateConst(0x85ebu))),
				                                                     HalfRateConst(7)),
				                                HalfRateConst(7));
				// Only in EXEC lanes, like the load it replaces: lanes below the always-append start
				// are off here and keep v3 = 1 (their append flag).
				const auto live = ir.GetExec();
				const auto set  = [&](uint32_t r, IR::U32 value) {
					const auto reg = static_cast<IR::VectorReg>(r);
					ir.SetVectorReg(reg, ir.Select(live, value, ir.GetVectorReg(reg)));
				};
				for (uint32_t r = 3; r <= 5; r++) set(r, HalfRateConst(0));
				set(6, ir.Select(ir.INotEqual(hash, HalfRateConst(0)), one_e30, minus_e30));
			}
			if (inst.pc == 0x4a4u) {
				// EXEC holds the items the guest appends.
				const auto bit = ir.ShiftLeftLogical(HalfRateConst(1), ir.BitwiseAnd(item, HalfRateConst(31)));
				const auto at  = ir.IAdd(check_word(H::kCheckLdsMask), ir.ShiftLeftLogical(ir.ShiftRightLogical(item, HalfRateConst(5)), HalfRateConst(2)));
				ir.Emit(O::SharedAtomicOr32, {at, bit, ir.GetExec()}, AddMemoryInfo(LdsWord(), current_pc));
			}
		}
	};
	pass(0);
	if (half_rate->half) {
		const auto& split = half_rate->splits[1];
		for (uint32_t k = 1; k < H::kCullPasses; k++) {
			auto*      from  = ir.CurrentBlock();
			const auto taken = ir.ULessThan(HalfRateConst(k * H::kThreads), designed);
			from->condition  = taken;
			ir.Emit(O::Reference, {taken});
			ir.SetBlock(split.pass[k - 1u]);
			pass(k);
			ir.SetBlock(split.tail[k - 1u]);
		}
	}
	ir.Emit(O::Barrier);

	const auto inside = ir.LogicalAnd(ir.ULessThan(tile_x, HalfRateConst(H::kCheckTilesX)),
	                                  ir.ULessThan(tile_y, HalfRateConst(H::kCheckTilesY)));
	const auto write  = ir.LogicalAnd(lead, inside);
	ir.SetExec(write);
	const auto base = ir.IAdd(HalfRateConst(half_rate->capture_region * H::kCheckRegionDwords),
	                          ir.IMul(tile, HalfRateConst(H::kCheckEntryDwords)));
	const auto read = [&](IR::U32 byte) { return IR::U32(LoadSharedU32(1, byte, LdsWord(), current_pc)); };
	const auto log  = [&](uint32_t word, IR::U32 value) {
		ir.Emit(O::DebugLogStoreU32, {ir.IAdd(base, HalfRateConst(word)), value, write});
	};
	log(0, ir.IAdd(read(HalfRateConst(H::kListCounterByte)), HalfRateConst(1)));
	for (uint32_t i = 0; i < H::kListEntries; i++) {
		log(1 + i, read(HalfRateConst(H::kListEntriesByte + i * 4u)));
	}
	log(H::kCheckWordItems, item_count);
	log(H::kCheckWordDesigned, designed);
	log(H::kCheckWordStart, start);
	log(H::kCheckWordStressN, read(check_word(H::kCheckLdsCounter)));
	for (uint32_t i = 0; i < H::kForcedItems; i++) {
		log(H::kCheckWordStress + i, read(check_word(H::kCheckLdsList + i)));
	}
	for (uint32_t w = 0; w < H::kCheckMaskDwords; w++) {
		log(H::kCheckWordMask + w, read(check_word(H::kCheckLdsMask + w)));
	}

	for (uint32_t r = 0; r < saved_v.size(); r++) ir.SetVectorReg(static_cast<IR::VectorReg>(3u + r), saved_v[r]);
	for (const auto& [r, value]: saved_s) ir.SetScalarReg(static_cast<IR::ScalarReg>(r), value);
	ir.SetVccLo(saved_vcc_lo);
	ir.SetVccHi(saved_vcc_hi);
	ir.SetScc(saved_scc);
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
	// Check mode: the pixel's own material flags and albedo for the adaptive-rate estimate.
	IR::U32                own_material {};
	std::array<IR::F32, 4> own_albedo {};
	auto                   own_albedo_energy = HalfRateFloat(0.0f);
	if (half_rate->capture) {
		TranslateInstruction(half_rate->material_desc);
		TranslateInstruction(half_rate->material_load);
		own_material = ir.GetVectorReg(static_cast<IR::VectorReg>(0));
		TranslateInstruction(half_rate->albedo_desc);
		TranslateInstruction(half_rate->albedo_load);
		for (uint32_t k = 0; k < 4u; k++) {
			own_albedo[k]     = f(ir.GetVectorReg(static_cast<IR::VectorReg>(6u + k)));
			own_albedo_energy = add(own_albedo_energy, mul(own_albedo[k], own_albedo[k]));
		}
	}
	auto luma_min        = HalfRateFloat(3.0e38f);
	auto luma_max        = HalfRateFloat(0.0f);
	auto any_same_mat    = IR::U1(IR::Value(false));
	auto any_other_mat   = IR::U1(IR::Value(false));
	auto best_albedo     = HalfRateFloat(0.0f);

	const auto ftx = IR::F32(ir.Emit(O::ConvertF32U32, {tx}));
	const auto fty = IR::F32(ir.Emit(O::ConvertF32U32, {ty}));

	std::array<IR::F32, 4> color {};
	color.fill(HalfRateFloat(0.0f));
	auto luma   = HalfRateFloat(0.0f);
	auto weight = HalfRateFloat(0.0f);
	auto best   = HalfRateFloat(0.0f); // best same-surface similarity of a valid candidate
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
			best = HalfRateFloatOp(O::FPMax32, best,
			                       IR::F32(ir.Emit(O::SelectF32, {valid, mul(depth_w, g_w), HalfRateFloat(0.0f)})));
			if (half_rate->capture || half_rate->adaptive || half_rate->retrace) {
				std::array<IR::F32, 3> rgb {};
				for (uint32_t k = 0; k < 3u; k++) rgb[k] = f(HalfRateLoad(Color + k, index));
				const auto lum = add(add(mul(rgb[0], HalfRateFloat(0.2126f)), mul(rgb[1], HalfRateFloat(0.7152f))),
				                     mul(rgb[2], HalfRateFloat(0.0722f)));
				luma_min = IR::F32(ir.Emit(O::SelectF32, {valid, HalfRateFloatOp(O::FPMin32, luma_min, lum), luma_min}));
				luma_max = IR::F32(ir.Emit(O::SelectF32, {valid, HalfRateFloatOp(O::FPMax32, luma_max, lum), luma_max}));
			}
			if (half_rate->capture) {
				const auto mat  = HalfRateCheckLoad(H::kCheckLdsMaterial, index);
				const auto same = ir.IEqual(mat, own_material);
				any_same_mat    = ir.LogicalOr(any_same_mat, ir.LogicalAnd(valid, same));
				any_other_mat   = ir.LogicalOr(any_other_mat, ir.LogicalAnd(valid, ir.LogicalNot(same)));
				auto a_diff   = HalfRateFloat(0.0f);
				auto a_energy = own_albedo_energy;
				for (uint32_t k = 0; k < 4u; k++) {
					const auto a = f(HalfRateCheckLoad(H::kCheckLdsAlbedo + k * H::kSamples, index));
					const auto d = sub(a, own_albedo[k]);
					a_diff       = add(a_diff, mul(d, d));
					a_energy     = add(a_energy, mul(a, a));
				}
				const auto a_w = falloff(mul(mul(a_diff, rcp(add(a_energy, HalfRateFloat(1e-4f)))), HalfRateFloat(kGbufferSharpness)));
				best_albedo    = IR::F32(ir.Emit(O::SelectF32, {valid, HalfRateFloatOp(O::FPMax32, best_albedo, a_w), best_albedo}));
			}
			for (uint32_t k = 0; k < 4u; k++) {
				color[k] = IR::F32(ir.Emit(O::FPFma32, {w, f(HalfRateLoad(Color + k, index)), color[k]}));
			}
			luma   = IR::F32(ir.Emit(O::FPFma32, {w, f(HalfRateLoad(Luma, index)), luma}));
			weight = add(weight, w);
		}
	}
	// Lit-luminance spread among the valid candidates (an edge the samples straddle).
	const auto spread        = mul(sub(luma_max, luma_min), rcp(HalfRateFloatOp(O::FPMax32, luma_max, HalfRateFloat(1e-3f))));
	const auto has_candidate = IR::U1(ir.Emit(O::FPOrdGreaterThanEqual32, {luma_max, luma_min}));
	if (half_rate->capture) {
		// KYTY_RT_HALF_CHECK: count unshaded pixels without a same-surface candidate.
		const auto count = [&](uint32_t counter, IR::U1 flag) {
			ir.Emit(O::SharedAtomicIAdd32,
			        {HalfRateConst(half_rate->check_base_bytes + (H::kCheckLdsRejected + counter) * 4u), HalfRateConst(1),
			         ir.LogicalAnd(ir.GetExec(), flag)},
			        AddMemoryInfo(LdsWord(), current_pc));
		};
		for (uint32_t t = 0; t < H::kRejectThresholds.size(); t++) {
			count(t, IR::U1(ir.Emit(O::FPOrdLessThan32, {best, HalfRateFloat(H::kRejectThresholds[t])})));
		}

		for (uint32_t t = 0; t < H::kSpreadThresholds.size(); t++) {
			count(3u + t, ir.LogicalAnd(has_candidate,
			                            IR::U1(ir.Emit(O::FPOrdGreaterThan32, {spread, HalfRateFloat(H::kSpreadThresholds[t])}))));
		}
		count(6, ir.LogicalAnd(has_candidate, ir.LogicalNot(any_same_mat)));
		count(7, any_other_mat);
		count(8, ir.LogicalAnd(has_candidate, IR::U1(ir.Emit(O::FPOrdLessThan32, {best_albedo, HalfRateFloat(0.5f)}))));
	}
	const auto any   = IR::U1(ir.Emit(O::FPOrdGreaterThan32, {weight, HalfRateFloat(0.0f)}));
	const auto scale = IR::F32(ir.Emit(O::SelectF32, {any, rcp(HalfRateFloatOp(O::FPMax32, weight, HalfRateFloat(1e-37f))), HalfRateFloat(0.0f)}));
	for (uint32_t k = 0; k < 4u; k++) {
		ir.SetVectorReg(static_cast<IR::VectorReg>(3u + k), ir.BitCastU32(mul(color[k], scale)));
	}
	ir.SetVectorReg(static_cast<IR::VectorReg>(8), ir.BitCastU32(mul(luma, scale)));
	if (half_rate->adaptive || half_rate->retrace) {
		// rt_half=2: an edge pixel is listed for the re-trace pass instead of being filled.
		// Diagnostic: rt_half=2 with rt_fill=9 keeps the loop but lists no pixel (one trip).
		const float threshold = half_rate->fill_mode == H::kFillRing9 ? 3.0e38f : H::kEdgeSpread;
		const auto edge = ir.LogicalAnd(ir.GetExec(), ir.LogicalAnd(has_candidate,
		                      IR::U1(ir.Emit(O::FPOrdGreaterThan32, {spread, HalfRateFloat(threshold)}))));
		const auto slot = IR::U32(ir.Emit(O::SharedAtomicIAdd32, {HalfRateConst(half_rate->adaptive_base_bytes), HalfRateConst(1), edge},
		                                  AddMemoryInfo(LdsWord(), current_pc)));
		const auto listed = ir.LogicalAnd(edge, ir.ULessThan(slot, HalfRateConst(H::kAdaptiveListMax)));
		const auto exec   = ir.GetExec();
		ir.SetExec(listed);
		WriteSharedU32(1, ir.IAdd(HalfRateConst(half_rate->adaptive_base_bytes + 16u), ir.ShiftLeftLogical(slot, HalfRateConst(2))),
		               {ir.BitwiseOr(tx, ir.ShiftLeftLogical(ty, HalfRateConst(4))), IR::Value(0u), IR::Value(0u), IR::Value(0u)},
		               LdsWord(), current_pc);
		if (half_rate->retrace && std::getenv("KYTY_RT_HALF_RETRACE_RED") != nullptr) {
			// Diagnostic: listed pixels are stored green by pass 1 (pass 2 overwrites them red).
			for (uint32_t k = 0; k < 4u; k++) {
				const auto reg = static_cast<IR::VectorReg>(3u + k);
				ir.SetVectorReg(reg, ir.Select(listed, HalfRateConst(k == 1 ? 0x40800000u : k == 3 ? 0x3f800000u : 0u),
				                               ir.GetVectorReg(reg)));
			}
			ir.SetExec(exec);
		} else {
			ir.SetExec(ir.LogicalAnd(exec, ir.LogicalNot(listed)));
		}
	}
	TranslateInstruction(half_rate->color_desc);
	TranslateInstruction(half_rate->color_store);
	TranslateInstruction(half_rate->luma_desc);
	TranslateInstruction(half_rate->luma_store);
}

// Before s_endpgm: all samples of the tile are in LDS after the barrier; each thread fills the
// right, lower and diagonal pixel of its 2x2.
void Translator::HalfRateReconstruct() {
	if (!half_rate->adaptive && !half_rate->retrace &&
	    (half_rate->fill_mode == H::kFillOff || half_rate->fill_mode == H::kFillQuarter)) {
		return;
	}
	ir.Emit(IR::ValueOpcode::Barrier);
	const auto live = ir.IEqual(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegTileLive)), HalfRateConst(1));
	HalfRateFill(1, 0, live);
	HalfRateFill(0, 1, live);
	HalfRateFill(1, 1, live);
	if (half_rate->capture) {
		// Thread 0 logs the tile's rejected-pixel counts (the tile origin is in kRegOrigin*).
		using O = IR::ValueOpcode;
		ir.Emit(O::Barrier);
		const auto tile_x = ir.ShiftRightLogical(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX)), HalfRateConst(4));
		const auto tile_y = ir.ShiftRightLogical(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY)), HalfRateConst(4));
		const auto lead   = ir.LogicalAnd(ir.IEqual(HalfRateLocal(0), HalfRateConst(0)), ir.IEqual(HalfRateLocal(1), HalfRateConst(0)));
		const auto write  = ir.LogicalAnd(ir.LogicalAnd(lead, live),
		                                  ir.LogicalAnd(ir.ULessThan(tile_x, HalfRateConst(H::kCheckTilesX)),
		                                                ir.ULessThan(tile_y, HalfRateConst(H::kCheckTilesY))));
		ir.SetExec(write);
		const auto base = ir.IAdd(HalfRateConst(half_rate->capture_region * H::kCheckRegionDwords),
		                          ir.IMul(ir.IAdd(ir.IMul(tile_y, HalfRateConst(H::kCheckTilesX)), tile_x),
		                                  HalfRateConst(H::kCheckEntryDwords)));
		for (uint32_t t = 0; t < H::kRejectCounters; t++) {
			const auto value = IR::U32(LoadSharedU32(1, HalfRateConst(half_rate->check_base_bytes + (H::kCheckLdsRejected + t) * 4u),
			                                         LdsWord(), current_pc));
			ir.Emit(O::DebugLogStoreU32, {ir.IAdd(base, HalfRateConst(H::kCheckWordRejected + t)), value, write});
		}
	}
}

IR::U32 Translator::HalfRateWorkgroupId() {
	return IR::U32(ir.Emit(IR::ValueOpcode::GetBuiltin,
	                       {IR::Value(static_cast<uint32_t>(IR::StageInputKind::WorkgroupId)), IR::Value(0u)}));
}

// The re-trace buffer word where workgroup `group`'s edge list starts.
IR::U32 Translator::HalfRateRetraceList(IR::U32 group) {
	const auto safe = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {group, HalfRateConst(H::kRetraceGroups - 1u)}));
	return ir.IAdd(HalfRateConst(H::kRetraceListsWord), ir.IMul(safe, HalfRateConst(H::kRetraceListDwords)));
}

// rt_half=2 pass 1, after the fill: the workgroup's edge list goes to the re-trace buffer, and each
// 64-pixel chunk of it is appended to the compact list that pass 2 dispatches over.
void Translator::HalfRateExportList() {
	using O = IR::ValueOpcode;
	ir.Emit(O::Barrier); // the LDS edge list is complete
	const auto group = HalfRateWorkgroupId();
	const auto base  = HalfRateRetraceList(group);
	const auto count = IR::U32(LoadSharedU32(1, HalfRateConst(half_rate->adaptive_base_bytes), LdsWord(), current_pc));
	const auto fits  = ir.ULessThan(group, HalfRateConst(H::kRetraceGroups));
	const auto live  = ir.IEqual(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegTileLive)), HalfRateConst(1));
	const auto thread = HalfRateFlatIndex();
	const auto exec   = ir.GetExec();
	for (uint32_t r = 0; r < H::kAdaptiveListMax / H::kThreads; r++) {
		const auto index = ir.IAdd(thread, HalfRateConst(r * H::kThreads));
		const auto take  = ir.LogicalAnd(ir.LogicalAnd(fits, live), ir.ULessThan(index, count));
		ir.SetExec(take);
		const auto packed = IR::U32(LoadSharedU32(1, ir.IAdd(HalfRateConst(half_rate->adaptive_base_bytes + 16u),
		                                                    ir.ShiftLeftLogical(index, HalfRateConst(2))),
		                                          LdsWord(), current_pc));
		ir.Emit(O::RetraceStoreU32, {ir.IAdd(ir.IAdd(base, HalfRateConst(4)), index), packed, take});
	}
	const auto lead = ir.LogicalAnd(ir.LogicalAnd(fits, live), ir.LogicalAnd(ir.IEqual(HalfRateLocal(0), HalfRateConst(0)),
	                                                                       ir.IEqual(HalfRateLocal(1), HalfRateConst(0))));
	ir.SetExec(lead);
	ir.Emit(O::RetraceStoreU32, {base, count, lead});
	for (uint32_t chunk = 0; chunk < H::kAdaptiveListMax / H::kThreads; chunk++) {
		const auto any = ir.LogicalAnd(lead, ir.ULessThan(HalfRateConst(chunk * H::kThreads), count));
		ir.Emit(O::RetraceAppendU32, {ir.BitwiseOr(ir.ShiftLeftLogical(group, HalfRateConst(2)), HalfRateConst(chunk)), any});
	}
	ir.SetExec(exec);
}

// rt_half=2 pass 2, before the guest reads its workgroup id (pc 0x018): take the guest workgroup of
// this dispatch's compact-list entry, so the tile's prologue and culling run as for that tile.
void Translator::HalfRateRetraceGroup() {
	const auto entry = IR::U32(ir.Emit(IR::ValueOpcode::RetraceLoadU32,
	                                   {ir.IAdd(HalfRateConst(4), IR::U32(ir.Emit(IR::ValueOpcode::UMin32,
	                                                                             {HalfRateWorkgroupId(), HalfRateConst(H::kRetraceEntries - 1u)})))}));
	const auto group = ir.ShiftRightLogical(entry, HalfRateConst(2));
	ir.SetScalarReg(static_cast<IR::ScalarReg>(16), group);
	ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(16), IR::U1(IR::Value(false)));
	ir.SetVectorReg(static_cast<IR::VectorReg>(kRegRetraceGroup), group);
	ir.SetVectorReg(static_cast<IR::VectorReg>(kRegRetraceChunk), ir.BitwiseAnd(entry, HalfRateConst(3)));
}

// rt_half=2 pass 2, after the pixel coordinates are formed: thread i shades listed pixel
// 64 * chunk + i; threads past the list get coordinates the guest's bounds check rejects.
void Translator::HalfRateRetraceRemap() {
	const auto origin_x = ir.GetScalarReg(static_cast<IR::ScalarReg>(12));
	const auto origin_y = ir.GetScalarReg(static_cast<IR::ScalarReg>(13));
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX), origin_x);
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY), origin_y);
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegTileLive), HalfRateConst(1));
	const auto group  = ir.GetVectorReg(static_cast<IR::VectorReg>(kRegRetraceGroup));
	const auto chunk  = ir.GetVectorReg(static_cast<IR::VectorReg>(kRegRetraceChunk));
	const auto base   = HalfRateRetraceList(group);
	const auto count  = IR::U32(ir.Emit(IR::ValueOpcode::RetraceLoadU32, {base}));
	const auto index  = ir.IAdd(ir.ShiftLeftLogical(chunk, HalfRateConst(6)), HalfRateFlatIndex());
	const auto safe   = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {index, HalfRateConst(H::kAdaptiveListMax - 1u)}));
	const auto packed = IR::U32(ir.Emit(IR::ValueOpcode::RetraceLoadU32, {ir.IAdd(ir.IAdd(base, HalfRateConst(4)), safe)}));
	const auto listed = ir.ULessThan(index, count);
	const auto outside = HalfRateConst(0xffffffffu);
	const auto x = ir.IAdd(origin_x, ir.BitwiseAnd(packed, HalfRateConst(15)));
	const auto y = ir.IAdd(origin_y, ir.BitwiseAnd(ir.ShiftRightLogical(packed, HalfRateConst(4)), HalfRateConst(15)));
	ir.SetVectorReg(static_cast<IR::VectorReg>(24), ir.Select(listed, x, outside));
	ir.SetVectorReg(static_cast<IR::VectorReg>(25), ir.Select(listed, y, outside));
}

// rt_half=2 diagnostic (KYTY_RT_HALF_CHECK): in re-trace iterations, the first active lane of the
// wave logs the guest light loop's state (pc 0x1de4: s33 < v17, v17 = s32 + v0, v0 = LDS count;
// pc 0x1df0: s10 = readfirstlane(v0)) at its 1st and 64th trip, into the tile's half-region entry
// (words 0..15 and 16..31; the half region is otherwise unused by rt_half=2).
void Translator::HalfRateLightLoopDump(uint32_t pc) {
	using O = IR::ValueOpcode;
	const auto counter_reg = static_cast<IR::VectorReg>(kRegLightIterations);
	auto       trips       = ir.GetVectorReg(counter_reg);
	if (pc == 0x1de4u) {
		trips = ir.Select(ir.GetExec(), ir.IAdd(trips, HalfRateConst(1)), trips);
		ir.SetVectorReg(counter_reg, trips);
	}
	const auto iteration = ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegIteration));
	const auto flat      = HalfRateFlatIndex();
	const auto first     = ir.IEqual(flat, IR::U32(ir.Emit(O::ReadFirstLane, {flat, ir.GetExec()})));
	const auto retrace   = ir.LogicalNot(ir.IEqual(iteration, HalfRateConst(0)));
	const auto at_1      = ir.IEqual(trips, HalfRateConst(1));
	const auto at_64     = ir.IEqual(trips, HalfRateConst(64));
	const auto tile_x    = ir.ShiftRightLogical(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX)), HalfRateConst(4));
	const auto tile_y    = ir.ShiftRightLogical(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY)), HalfRateConst(4));
	const auto inside    = ir.LogicalAnd(ir.ULessThan(tile_x, HalfRateConst(H::kCheckTilesX)), ir.ULessThan(tile_y, HalfRateConst(H::kCheckTilesY)));
	const auto write     = ir.LogicalAnd(ir.LogicalAnd(ir.GetExec(), first), ir.LogicalAnd(retrace, ir.LogicalAnd(inside, ir.LogicalOr(at_1, at_64))));
	const auto base      = ir.IAdd(ir.IAdd(HalfRateConst(H::kCheckRegionHalf * H::kCheckRegionDwords),
	                                       ir.IMul(ir.IAdd(ir.IMul(tile_y, HalfRateConst(H::kCheckTilesX)), tile_x), HalfRateConst(H::kCheckEntryDwords))),
	                              ir.Select(at_64, HalfRateConst(16), HalfRateConst(0)));
	const auto log = [&](uint32_t word, IR::U32 value) {
		ir.Emit(O::DebugLogStoreU32, {ir.IAdd(base, HalfRateConst(word)), value, write});
	};
	const auto exec_mask = BallotMask(ir.GetExec());
	if (pc == 0x1de4u) {
		const auto vcc_mask = BallotMask(ir.GetVcc());
		log(0, ir.IAdd(iteration, HalfRateConst(0x100u))); // marker: 0x100 + loop iteration
		log(1, trips);
		log(2, ir.GetScalarReg(static_cast<IR::ScalarReg>(33)));
		log(3, ir.GetVectorReg(static_cast<IR::VectorReg>(17)));
		log(4, ir.GetVectorReg(static_cast<IR::VectorReg>(0)));
		log(5, ir.GetScalarReg(static_cast<IR::ScalarReg>(32)));
		log(6, exec_mask[0]);
		log(7, exec_mask[1]);
		log(8, vcc_mask[0]);
		log(9, vcc_mask[1]);
		log(10, flat);
		log(11, ir.GetVectorReg(static_cast<IR::VectorReg>(24)));
		log(12, ir.GetVectorReg(static_cast<IR::VectorReg>(25)));
	} else {
		log(13, ir.GetScalarReg(static_cast<IR::ScalarReg>(10)));
		log(14, ir.GetScalarReg(static_cast<IR::ScalarReg>(35)));
		log(15, ir.GetScalarReg(static_cast<IR::ScalarReg>(36)));
	}
}

// rt_half=2, after the list barrier (in the pre block): save the registers the lighting body
// starts from, clear the re-trace list, and enter the loop header. Iteration 0 shades the
// even/even samples (coordinates from HalfRateRemap). Iteration k >= 1 shades listed edge pixels
// (k-1)*64 + thread, and threads past the list get coordinates the guest's bounds check rejects.
void Translator::HalfRateLoopEnter() {
	auto& saved = half_rate->loop_saved;
	saved.v.clear();
	saved.s.clear();
	for (uint32_t r = 0; r < HalfRatePlan::kLoopSavedVgprs; r++) saved.v.push_back(ir.GetVectorReg(static_cast<IR::VectorReg>(r)));
	saved.thread_bits.clear();
	saved.mask_tags.clear();
	for (uint32_t r = 0; r < HalfRatePlan::kLoopSavedSgprs; r++) {
		const auto reg = static_cast<IR::ScalarReg>(r);
		saved.s.push_back(ir.GetScalarReg(reg));
		saved.thread_bits.push_back(ir.GetThreadBitScalarReg(reg));
		saved.mask_tags.push_back(ir.GetScalarMaskTag(reg));
	}
	saved.vcc_lo = ir.GetVccLo();
	saved.vcc_hi = ir.GetVccHi();
	saved.scc    = ir.GetScc();
	saved.m0     = ir.GetM0();
	// The CFG structurizer's goto variables: an iteration must start with the values the first one
	// saw (a variable set only before the body would otherwise carry the last iteration's value).
	saved.goto_values.clear();
	for (const auto id: half_rate->goto_variables) saved.goto_values.push_back(ir.GetGotoVariable(id));
	const auto exec = ir.GetExec();
	const auto lead = ir.LogicalAnd(ir.IEqual(HalfRateLocal(0), HalfRateConst(0)), ir.IEqual(HalfRateLocal(1), HalfRateConst(0)));
	ir.SetExec(lead);
	WriteSharedU32(1, HalfRateConst(half_rate->adaptive_base_bytes), {HalfRateConst(0), IR::Value(0u), IR::Value(0u), IR::Value(0u)},
	               LdsWord(), current_pc);
	ir.SetExec(exec);
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegIteration), HalfRateConst(0));

	const auto& loop = half_rate->loop;
	ir.SetBlock(loop.header);
	const auto iteration = ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegIteration));
	const auto first     = ir.IEqual(iteration, HalfRateConst(0));
	const auto count     = IR::U32(LoadSharedU32(1, HalfRateConst(half_rate->adaptive_base_bytes), LdsWord(), current_pc));
	const auto index     = ir.IAdd(ir.IMul(ir.ISub(iteration, HalfRateConst(1)), HalfRateConst(H::kThreads)), HalfRateFlatIndex());
	const auto safe      = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {index, HalfRateConst(H::kAdaptiveListMax - 1u)}));
	const auto packed    = IR::U32(LoadSharedU32(1, ir.IAdd(HalfRateConst(half_rate->adaptive_base_bytes + 16u),
	                                                       ir.ShiftLeftLogical(safe, HalfRateConst(2))),
	                                             LdsWord(), current_pc));
	const auto listed    = ir.ULessThan(index, count);
	const auto outside   = HalfRateConst(0xffffffffu);
	const auto x = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginX)), ir.BitwiseAnd(packed, HalfRateConst(15)));
	const auto y = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegOriginY)),
	                       ir.BitwiseAnd(ir.ShiftRightLogical(packed, HalfRateConst(4)), HalfRateConst(15)));
	const auto v24 = ir.GetVectorReg(static_cast<IR::VectorReg>(24));
	const auto v25 = ir.GetVectorReg(static_cast<IR::VectorReg>(25));
	// Diagnostic: rt_half=2 with rt_fill=0 lists edge pixels but re-traces none (iterations run, every
	// thread outside the screen).
	const auto take = half_rate->fill_mode == H::kFillOff ? IR::U1(IR::Value(false)) : listed;
	ir.SetVectorReg(static_cast<IR::VectorReg>(24), ir.Select(first, v24, ir.Select(take, x, outside)));
	ir.SetVectorReg(static_cast<IR::VectorReg>(25), ir.Select(first, v25, ir.Select(take, y, outside)));
	ir.SetExec(IR::U1(IR::Value(true)));
	ir.SetBlock(loop.body_entry);
}

// rt_half=2, before s_endpgm: the body's exits meet in the merge block. The first iteration fills
// (listing edge pixels), the latch restores the body's starting registers and loops while listed
// pixels remain; the exit block leads to the s_endpgm block.
void Translator::HalfRateLoopExit() {
	using O = IR::ValueOpcode;
	const auto& loop = half_rate->loop;
	ir.SetBlock(loop.merge);
	const auto iteration = ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegIteration));
	const auto first     = ir.IEqual(iteration, HalfRateConst(0));
	loop.merge->condition = first;
	ir.Emit(O::Reference, {first});

	ir.SetBlock(loop.fill);
	HalfRateReconstruct();
	ir.Emit(O::Barrier); // the list count is final

	ir.SetBlock(loop.fill_merge);
	ir.SetBlock(loop.latch);
	const auto& saved = half_rate->loop_saved;
	for (uint32_t r = 0; r < saved.v.size(); r++) ir.SetVectorReg(static_cast<IR::VectorReg>(r), saved.v[r]);
	for (uint32_t r = 0; r < saved.s.size(); r++) {
		const auto reg = static_cast<IR::ScalarReg>(r);
		ir.SetScalarReg(reg, saved.s[r]);
		ir.SetThreadBitScalarReg(reg, saved.thread_bits[r]);
		ir.SetScalarMaskTag(reg, saved.mask_tags[r]);
	}
	ir.SetVccLo(saved.vcc_lo);
	ir.SetVccHi(saved.vcc_hi);
	ir.SetScc(saved.scc);
	ir.SetM0(saved.m0);
	for (size_t i = 0; i < saved.goto_values.size(); i++) ir.SetGotoVariable(half_rate->goto_variables[i], saved.goto_values[i]);
	ir.SetExec(IR::U1(IR::Value(true)));
	const auto next  = ir.IAdd(ir.GetVectorReg(static_cast<IR::VectorReg>(H::kRegIteration)), HalfRateConst(1));
	ir.SetVectorReg(static_cast<IR::VectorReg>(H::kRegIteration), next);
	const auto count = IR::U32(LoadSharedU32(1, HalfRateConst(half_rate->adaptive_base_bytes), LdsWord(), current_pc));
	auto trips = ir.IAdd(HalfRateConst(1), ir.ShiftRightLogical(ir.IAdd(count, HalfRateConst(H::kThreads - 1u)), HalfRateConst(6)));
	if (half_rate->fill_mode == H::kFillQuarter) {
		// Diagnostic: rt_half=2 with rt_fill=99 re-traces at most the first 64 listed pixels.
		trips = IR::U32(ir.Emit(O::UMin32, {trips, HalfRateConst(2)}));
	}
	const auto again = ir.ULessThan(next, trips);
	loop.latch->condition = again;
	ir.Emit(O::Reference, {again});

	ir.SetBlock(loop.exit);
	ir.SetBlock(loop.end);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
