// DS_ORDERED_COUNT's lane election (frontend/translate/LaneElection.h): KYTY_PS_LIVE_ELECT elects
// a non-helper lane; without it the IR is the plain lowest-EXEC-lane election.
#include "graphics/shader/recompiler/frontend/translate/LaneElection.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/color.h>
#include <string_view>
#include <vector>

namespace {

void Check(bool c, const char* t) {
	if (!c) {
		std::fprintf(stderr, "LaneElectionTests: %s\n", t);
		std::abort();
	}
}

using namespace Libs::Graphics::ShaderRecompiler;
using IR::ValueOpcode;

std::vector<const IR::Inst*> Find(const IR::Block& block, ValueOpcode opcode) {
	std::vector<const IR::Inst*> found;
	for (const auto& inst: block) {
		if (inst.GetOpcode() == opcode) {
			found.push_back(&inst);
		}
	}
	return found;
}

void TestPlainElection() {
	IR::Block      block;
	IR::IREmitter  ir(&block);
	const auto     exec     = ir.INotEqual(IR::U32(ir.Emit(ValueOpcode::LaneId)), IR::U32(IR::Value(3u)));
	const auto     before   = Find(block, ValueOpcode::IsHelperInvocation).size();
	const auto     election = Frontend::ElectFirstLane(ir, exec, false);
	Check(Find(block, ValueOpcode::IsHelperInvocation).size() == before, "disabled: no helper query");
	const auto ballots = Find(block, ValueOpcode::Ballot);
	Check(ballots.size() == 1 && ballots[0]->Arg(0) == IR::Value(exec), "disabled: one ballot of EXEC");
	Check(!election.active.IsEmpty() && !election.selected.IsEmpty(), "disabled: results set");
	const auto* selected = election.selected.TryInstruction();
	Check(selected != nullptr && selected->GetOpcode() == ValueOpcode::SelectU32 &&
	          selected->Arg(0) == IR::Value(election.active),
	      "disabled: lane 0 when EXEC is empty");
}

void TestLiveElection() {
	IR::Block     block;
	IR::IREmitter ir(&block);
	// EXEC: every lane but lane 3.
	const auto    exec     = ir.INotEqual(IR::U32(ir.Emit(ValueOpcode::LaneId)), IR::U32(IR::Value(3u)));
	const auto    election = Frontend::ElectFirstLane(ir, exec, true);
	const auto helpers = Find(block, ValueOpcode::IsHelperInvocation);
	Check(helpers.size() == 1, "enabled: one helper query");
	const auto ballots = Find(block, ValueOpcode::Ballot);
	Check(ballots.size() == 2, "enabled: EXEC ballot and live-lane ballot");
	// The second ballot's predicate is EXEC && !helper.
	const auto* live = ballots[1]->Arg(0).TryInstruction();
	Check(live != nullptr && live->GetOpcode() == ValueOpcode::LogicalAnd, "enabled: live predicate is an AND");
	const auto* not_helper = live->Arg(1).TryInstruction();
	Check(not_helper != nullptr && not_helper->GetOpcode() == ValueOpcode::LogicalNot &&
	          not_helper->Arg(0).TryInstruction() == helpers[0],
	      "enabled: AND with !IsHelperInvocation");
	Check(live->Arg(0) == ballots[0]->Arg(0), "enabled: the same EXEC");
	// The elected lane prefers the live lane and falls back to the EXEC lane.
	const auto* selected = election.selected.TryInstruction();
	Check(selected != nullptr && selected->GetOpcode() == ValueOpcode::SelectU32, "enabled: select");
	const auto* fallback = selected->Arg(2).TryInstruction();
	Check(fallback != nullptr && fallback->GetOpcode() == ValueOpcode::SelectU32 &&
	          fallback->Arg(0) == IR::Value(election.active),
	      "enabled: falls back to the plain election");
}

} // namespace

namespace Common {
int DbgExitHandler(const char*, int, std::string_view) {
	std::abort();
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view) {
	std::abort();
}
int DbgExitIfHandler(const char*, const char*, int) {
	return 1;
}
void DbgExit(int) {
	std::abort();
}
} // namespace Common

int main() {
	TestPlainElection();
	TestLiveElection();
	std::puts("LaneElectionTests: ok");
	return 0;
}

#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/IREmitter.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
