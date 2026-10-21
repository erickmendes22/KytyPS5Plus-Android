#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_FRONTONLYPACKETS_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_FRONTONLYPACKETS_H_

#include "graphics/guest_gpu/pm4.h"

#include <cstdint>

// KYTY_GPU_PIPELINE: which PM4 packets the front handles without draining the record thread, and
// the bodies of the "front-only" packets that KYTY_DRAIN_LITE adds to that set. They are templates
// over the command processor and the context block so that tests/FrontOnlyPacketsTests.cpp runs
// the same code against a fake: the packet words are only read, and the only state written is the
// front's live context block and its push/pop copy (queued items carry their own register
// snapshot, gpuPipelineItems.h, so the record thread never reads the live block).
namespace Libs::Graphics::FrontOnlyPackets {

// Context-state operations of R_CONTEXT_STATE (ContextStateOperation in commandProcessor.h).
constexpr uint32_t kContextClear     = 0;
constexpr uint32_t kContextPush      = 1;
constexpr uint32_t kContextPop       = 2;
constexpr uint32_t kContextPushClear = 3;

// Packets the front always handles in place: they only update the command processor's own state
// (registers, index state, buffer chaining) or queue a draw/dispatch record job; anything else may
// touch renderer state the record thread owns and drains it first.
constexpr bool IsAlwaysPipelined(uint32_t opcode, uint32_t r) {
	switch (opcode) {
		case Pm4::IT_NOP: return r == 0; // custom ops (flips, markers, release_mem, ...) drain
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		// Register lists read from guest memory at parse time, through the same per-register
		// functions as the direct packets (b6_fan2: 0x64 167/frame and 0x9f 54/frame drained).
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
		case Pm4::IT_SET_SH_REG_INDIRECT:
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDEX_OFFSET_2:
		case Pm4::IT_DRAW_INDEX_AUTO:
		case Pm4::IT_DISPATCH_DIRECT:
		case Pm4::IT_INDIRECT_BUFFER: return true;
		default: return false;
	}
}

// KYTY_DRAIN_LITE: packets whose handlers below touch nothing but the front's context block.
// c14_prof3 (Astro Bot crash site): 76 of 128.5 drains/frame were these.
constexpr bool IsFrontOnly(uint32_t opcode, uint32_t r) {
	if (opcode == Pm4::IT_ACQUIRE_MEM) {
		return true;
	}
	return opcode == Pm4::IT_NOP &&
	       (r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER || r == Pm4::R_CONTEXT_STATE);
}

constexpr bool IsPipelined(uint32_t opcode, uint32_t r, bool drain_lite) {
	return IsAlwaysPipelined(opcode, r) || (drain_lite && IsFrontOnly(opcode, r));
}

// Dwords after the header that R_PUSH_MARKER / R_POP_MARKER consume (the marker string).
constexpr uint32_t MarkerBodyDwords(uint32_t cmd_id) {
	return ((cmd_id >> 16u) & 0x3fffu) + 1u;
}

// IT_ACQUIRE_MEM: the renderer orders memory with its own barriers, so the packet is skipped.
// False for a header the command processor does not implement.
constexpr bool AcquireMemValid(uint32_t cmd_id) {
	return cmd_id == 0xc0055800u || cmd_id == 0xc0065800u;
}

constexpr uint32_t AcquireMemBodyDwords(uint32_t cmd_id) {
	return KYTY_PM4_LEN(cmd_id) - 1u;
}

// R_CONTEXT_STATE on the front's context block `ctx` (a type with Reset()) and its saved copy.
// Returns false for an unknown operation or an unbalanced push/pop; nothing is changed then.
template <typename Context>
bool ApplyContextState(uint32_t operation, Context& ctx, Context& saved, bool& pushed) {
	switch (operation) {
		case kContextClear: ctx.Reset(); return true;
		case kContextPush:
		case kContextPushClear:
			if (pushed) {
				return false;
			}
			saved  = ctx;
			pushed = true;
			if (operation == kContextPushClear) {
				ctx.Reset();
			}
			return true;
		case kContextPop:
			if (!pushed) {
				return false;
			}
			ctx    = saved;
			saved  = Context {};
			pushed = false;
			return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::FrontOnlyPackets

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_FRONTONLYPACKETS_H_ */
