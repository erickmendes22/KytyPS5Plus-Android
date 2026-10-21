// KYTY_LOOP_GUARD: hash-list parsing (graphics/shader/loopGuard.h) and the choice of each
// loop's counting header, forced exit branch and log block (ir/passes/LoopGuardPlan.h).
#include "graphics/shader/loopGuard.h"
#include "graphics/shader/recompiler/ir/passes/LoopGuardPlan.h"

#include <cstdio>
#include <fmt/color.h>
#include <string_view>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

void Check(bool c, const char* t) {
	if (!c) {
		std::fprintf(stderr, "LoopGuardTests: %s\n", t);
		std::abort();
	}
}

using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;
using IR::Block;

struct Graph {
	std::vector<std::unique_ptr<Block>> owned;
	std::vector<Block*>                 blocks;
	Block* Add() {
		owned.push_back(std::make_unique<Block>());
		blocks.push_back(owned.back().get());
		blocks.back()->id = static_cast<uint32_t>(blocks.size() - 1);
		return blocks.back();
	}
};

void Branch(Block* from, Block* to) {
	from->terminator.kind       = CFG::TerminatorKind::Branch;
	from->terminator.true_block = to;
}
void Conditional(Block* from, Block* t, Block* f) {
	from->terminator.kind        = CFG::TerminatorKind::ConditionalBranch;
	from->terminator.true_block  = t;
	from->terminator.false_block = f;
	from->condition              = IR::Value(true);
}
void Loop(Block* header, Block* merge, Block* cont) {
	header->terminator.loop_header    = true;
	header->terminator.merge_block    = merge;
	header->terminator.continue_block = cont;
}

void TestMatches() {
	const uint64_t hash = 0x657ad04626bf9d55ull;
	Check(LoopGuard::Matches("657ad046", hash), "prefix matches");
	Check(LoopGuard::Matches("0x657AD046", hash), "0x and upper case");
	Check(LoopGuard::Matches("3a8772db:657ad046", hash) && LoopGuard::Matches("3a8772db,657a", hash), "lists");
	Check(LoopGuard::Matches("657ad04626bf9d55", hash), "full hash");
	Check(!LoopGuard::Matches("657ad04626bf9d551", hash), "longer than the hash");
	Check(!LoopGuard::Matches("57ad046", hash), "not a prefix");
	Check(!LoopGuard::Matches("", hash) && !LoopGuard::Matches("::", hash) && !LoopGuard::Matches(nullptr, hash),
	      "empty tokens never match");
	Check(LoopGuard::Matches("0x00000000", 0x1234ull), "leading zero digits");
	Check(LoopGuard::ParseLimit(nullptr) == LoopGuard::kDefaultLimit && LoopGuard::ParseLimit("") == 65536u,
	      "default limit");
	Check(LoopGuard::ParseLimit("0") == 1u && LoopGuard::ParseLimit("0x100") == 256u, "parsed limit");
	Check(LoopGuard::ParseLimit("99999999999") == UINT32_MAX - 1u, "limit clamps");
}

// entry -> H: if exec_zero -> B (break) -> M, else body -> C (continue) -> H. 657ad046's loop 1.
void TestHeaderExit() {
	Graph g;
	auto* entry = g.Add();
	auto* h     = g.Add();
	auto* b     = g.Add();
	auto* body  = g.Add();
	auto* c     = g.Add();
	auto* m     = g.Add();
	Branch(entry, h);
	Conditional(h, b, body);
	Loop(h, m, c);
	Branch(b, m);
	Branch(body, c);
	Branch(c, h);
	Branch(m, nullptr);
	const auto plan = IR::PlanLoopGuards(g.blocks);
	Check(plan.loops.size() == 1 && plan.unguarded.empty(), "one guardable loop");
	const auto& loop = plan.loops[0];
	Check(loop.header == h && loop.guard == h && loop.exit_on_true, "header branch exits on true");
	Check(loop.log_block == b, "the break block logs");
}

// do-while: H -> body -> C: if again -> H else -> M. The exit is the continue block's false edge,
// straight to the merge block, which logs.
void TestContinueExit() {
	Graph g;
	auto* h    = g.Add();
	auto* body = g.Add();
	auto* c    = g.Add();
	auto* m    = g.Add();
	Branch(h, body);
	Loop(h, m, c);
	Branch(body, c);
	Conditional(c, h, m);
	Branch(m, nullptr);
	const auto plan = IR::PlanLoopGuards(g.blocks);
	Check(plan.loops.size() == 1, "do-while guarded");
	Check(plan.loops[0].guard == c && !plan.loops[0].exit_on_true && plan.loops[0].log_block == m,
	      "continue block exits on false into the merge");
}

// Nested: the outer loop's exit is in its header; the inner loop exits from its own header; an
// inner merge that is itself a loop header never logs.
void TestNested() {
	Graph g;
	auto* oh  = g.Add(); // outer header
	auto* ob  = g.Add(); // outer break
	auto* ih  = g.Add(); // inner header
	auto* ibd = g.Add(); // inner body
	auto* ic  = g.Add(); // inner continue
	auto* im  = g.Add(); // inner merge = outer continue
	auto* om  = g.Add(); // outer merge
	Conditional(oh, ob, ih);
	Loop(oh, om, im);
	Branch(ob, om);
	Conditional(ih, im, ibd);
	Loop(ih, im, ic);
	Branch(ibd, ic);
	Branch(ic, ih);
	Branch(im, oh);
	Branch(om, nullptr);
	const auto plan = IR::PlanLoopGuards(g.blocks);
	Check(plan.loops.size() == 2, "both loops guarded");
	Check(plan.loops[0].header == oh && plan.loops[0].guard == oh && plan.loops[0].log_block == ob, "outer");
	Check(plan.loops[1].header == ih && plan.loops[1].guard == ih && plan.loops[1].exit_on_true &&
	          plan.loops[1].log_block == im,
	      "inner exits into its merge");
	// The inner merge as a loop header: still guarded, not logged.
	im->terminator.loop_header = true;
	im->terminator.merge_block = om;
	const auto again = IR::PlanLoopGuards(g.blocks);
	Check(again.loops.size() >= 2 && again.loops[1].log_block == nullptr, "a loop-header merge never logs");
}

// A loop whose only branches stay inside (no edge to the merge) is reported unguarded, as is a
// header whose merge block precedes it.
void TestUnguarded() {
	Graph g;
	auto* h    = g.Add();
	auto* body = g.Add();
	auto* m    = g.Add();
	Conditional(h, body, h);
	Loop(h, m, body);
	Branch(body, h);
	Branch(m, nullptr);
	auto plan = IR::PlanLoopGuards(g.blocks);
	Check(plan.loops.empty() && plan.unguarded.size() == 1, "no exit edge: unguarded");
	Graph back;
	auto* m2 = back.Add();
	auto* h2 = back.Add();
	Conditional(h2, m2, h2);
	Loop(h2, m2, h2);
	plan = IR::PlanLoopGuards(back.blocks);
	Check(plan.loops.empty() && plan.unguarded.size() == 1, "merge before header: unguarded");
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
	TestMatches();
	TestHeaderExit();
	TestContinueExit();
	TestNested();
	TestUnguarded();
	std::puts("LoopGuardTests: ok");
	return 0;
}

#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
