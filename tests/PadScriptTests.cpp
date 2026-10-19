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
	auto r = PadScript::Parse("1500:128,128,R=255/0;200:128,0,cross,R=10/20;100:128,128");
	Check(r && r->size() == 3 && (*r)[0].right_x == 255 && (*r)[0].right_y == 0 && (*r)[0].buttons == 0,
	      "right stick step");
	Check((*r)[1].buttons == PAD_BUTTON_CROSS && (*r)[1].right_x == 10 && (*r)[1].right_y == 20,
	      "buttons and right stick");
	Check((*r)[2].right_x == 128 && (*r)[2].right_y == 128, "right stick centred by default");
	Check(!PadScript::Parse("100:128,128,R=256/0"), "right axis range");
	Check(!PadScript::Parse("100:128,128,R=12"), "right stick needs x/y");
	std::puts("PadScriptTests: ok");
	return 0;
}
