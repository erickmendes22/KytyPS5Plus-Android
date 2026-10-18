// Parser for KYTY_LIVE_KNOBS pad= scripts (libs/padScript.h).
#include "libs/padScript.h"
#include <cstdio>
#include <cstdlib>
using namespace Libs::Controller;
static void Check(bool c, const char* t) { if (!c) { std::fprintf(stderr, "PadScriptTests: %s\n", t); std::abort(); } }
int main() {
	auto s = PadScript::Parse("3000:128,0;400:128,0,cross;2000:128,0, ");
	Check(s && s->size() == 3, "three steps");
	Check((*s)[0].ms == 3000 && (*s)[0].left_x == 128 && (*s)[0].left_y == 0 && (*s)[0].buttons == 0, "walk step");
	Check((*s)[1].buttons == PAD_BUTTON_CROSS, "cross step");
	auto m = PadScript::Parse("500:0,255,cross+r1+options");
	Check(m && (*m)[0].buttons == (PAD_BUTTON_CROSS | PAD_BUTTON_R1 | PAD_BUTTON_OPTIONS), "combined buttons");
	Check(!PadScript::Parse(""), "empty rejected");
	Check(!PadScript::Parse("100:256,0"), "axis range");
	Check(!PadScript::Parse("0:128,128"), "zero ms");
	Check(!PadScript::Parse("100:128"), "missing y");
	Check(!PadScript::Parse("100:128,0,jump"), "unknown button");
	Check(!PadScript::Parse("30000:128,0;30000:128,0;30000:128,0;30000:128,0;1:128,0"), "total limit");
	Check(PadScript::Parse(";;100:128,128;").has_value(), "blank steps skipped");
	std::puts("PadScriptTests: ok");
	return 0;
}
