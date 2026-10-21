#include "graphics/shader/recompiler/frontend/translate/LaneElection.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

struct FirstLane {
	IR::U1  any;
	IR::U32 lane;
};

FirstLane LowestLane(IR::IREmitter& ir, IR::U1 predicate) {
	const auto ballot = ir.Emit(IR::ValueOpcode::Ballot, {predicate});
	const auto low    = IR::U32(ir.CompositeExtract(ballot, 0u));
	const auto high   = IR::U32(ir.CompositeExtract(ballot, 1u));
	const auto any    = ir.INotEqual(ir.BitwiseOr(low, high), IR::U32(IR::Value(0u)));
	const auto lane   = ir.Select(
        ir.INotEqual(low, IR::U32(IR::Value(0u))), IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {low})),
        ir.IAdd(IR::U32(IR::Value(32u)), IR::U32(ir.Emit(IR::ValueOpcode::FindILsb32, {high}))));
	return {any, IR::U32(lane)};
}

} // namespace

LaneElection ElectFirstLane(IR::IREmitter& ir, IR::U1 exec, bool exclude_helpers) {
	const auto first = LowestLane(ir, exec);
	auto       lane  = IR::U32(ir.Select(first.any, first.lane, IR::U32(IR::Value(0u))));
	if (exclude_helpers) {
		const auto helper = IR::U1(ir.Emit(IR::ValueOpcode::IsHelperInvocation));
		const auto live   = LowestLane(ir, ir.LogicalAnd(exec, ir.LogicalNot(helper)));
		lane              = IR::U32(ir.Select(live.any, live.lane, lane));
	}
	return {first.any, lane};
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
