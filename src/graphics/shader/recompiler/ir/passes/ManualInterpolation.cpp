#include "graphics/shader/recompiler/ir/passes/ManualInterpolation.h"

#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

// What a value is, as a function of the three vertices' raw parameter dwords and the raw I/J.
enum class Kind : uint8_t {
	Independent, // No vertex data and no I/J (or not tracked: then never per-vertex).
	Const,       // A known bit pattern.
	Raw,         // One vertex's raw dword (or, `difference`, v_k - v_0 of a float parameter).
	Field,       // A bit field of one vertex's raw dword, before conversion.
	Form,        // sum_k c_k(I, J) * x(v_k) * factors + (terms without vertex data or I/J).
	Bary,        // a0 + a1 * I + a2 * J, times factors.
	Carried,     // Vertex data or I/J only in lanes a write skipped (exec merge, phi, DPP kept).
};

struct Attribute {
	uint32_t            param  = 0;
	uint32_t            chan   = 0;
	uint32_t            offset = 0;
	uint32_t            count  = 32;
	PackedVaryingFormat format = PackedVaryingFormat::Float32;
	bool                flat   = false; // the provoking vertex's raw dword

	bool operator==(const Attribute& other) const = default;
};

using Affine = std::array<double, 3>; // [1, I, J]

struct Abs {
	Kind     kind = Kind::Independent;
	uint32_t bits = 0; // Const
	// Raw / Field
	const Inst* source = nullptr; // the v_interp_mov read
	uint32_t param = 0, chan = 0, vertex = 0;
	bool     difference = false;
	uint32_t offset = 0, count = 32;
	bool     is_signed = false, negate = false;
	// Form / Bary
	int32_t                  attribute = -1;
	int32_t                  bary      = -1; // StageInputKind of the I/J pair, -1: none
	std::vector<const Inst*> factors;        // sorted
	std::array<Affine, 3>    c {};           // Form: per vertex; Bary: c[0]

	[[nodiscard]] bool PerVertex() const {
		return kind == Kind::Raw || kind == Kind::Field || kind == Kind::Form || kind == Kind::Bary ||
		       kind == Kind::Carried;
	}
};

float HalfToFloat(uint32_t h) {
	const uint32_t sign = (h >> 15u) & 1u, exponent = (h >> 10u) & 0x1fu, mantissa = h & 0x3ffu;
	float value;
	if (exponent == 0) {
		value = std::ldexp(static_cast<float>(mantissa), -24);
	} else if (exponent == 31) {
		value = mantissa == 0 ? INFINITY : NAN;
	} else {
		value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
	}
	return sign != 0 ? -value : value;
}

std::optional<uint32_t> FloatToHalfExact(float value) {
	// Only exactly representable values (the constants seen are 1.0); others stay unknown.
	for (uint32_t h = 0; h < 0x10000u; h += 1) {
		if ((h & 0x7c00u) == 0x7c00u) continue;
		if (HalfToFloat(h) == value && (std::signbit(value) == ((h & 0x8000u) != 0))) return h;
	}
	return std::nullopt;
}

float AsFloat(uint32_t bits) { return std::bit_cast<float>(bits); }
uint32_t AsBits(float value) { return std::bit_cast<uint32_t>(value); }

bool IsBaryKind(uint32_t kind) {
	switch (static_cast<StageInputKind>(kind)) {
		case StageInputKind::BaryCoordSmooth:
		case StageInputKind::BaryCoordSmoothSample:
		case StageInputKind::BaryCoordSmoothCentroid:
		case StageInputKind::BaryCoordNoPerspective: return true;
		default: return false;
	}
}

class Analyzer {
public:
	Analyzer(Program& program_, const ShaderPixelInputInfo& pixel_, bool drop_carried_)
	    : program(program_), pixel(pixel_), drop_carried(drop_carried_) {}

	bool Run() {
		for (auto* block: program.blocks) {
			for (auto& inst: *block) {
				Visit(inst);
				visited.insert(&inst);
				if (!reason.empty()) return false;
			}
		}
		for (const auto& [inst, operand]: deferred) {
			if (Get(operand).PerVertex()) {
				return Reject(fmt::format("vertex data reaches {} through a back edge",
				                          ValueOpcodeName(inst->GetOpcode())));
			}
		}
		for (const auto& [inst, value]: values) {
			if (!value.PerVertex()) continue;
			for (const auto& use: inst->Uses()) {
				if (use.user->Parent() == nullptr) return Reject("vertex data used outside the blocks");
			}
		}
		if (leaves.empty() && flat_reads.empty()) return Reject("no unpacked vertex values");
		return true;
	}

	Program&                                        program;
	const ShaderPixelInputInfo&                     pixel;
	bool                                            drop_carried = false;
	std::unordered_map<const Inst*, Abs>            values;
	std::unordered_set<const Inst*>                 visited;
	std::vector<std::pair<const Inst*, Value>>      deferred;
	std::vector<Attribute>                          attributes;
	std::vector<int32_t>                            attribute_bary;
	struct Leaf {
		int32_t attribute = -1;
		double  scale     = 0.0; // the leaf equals scale * x once every vertex reads x
	};
	std::unordered_map<Inst*, Leaf>                 leaves;
	// Kept operands (exec-merge selects, DPP updates) that carry vertex data or I/J only in
	// skipped lanes; with drop_carried they are replaced by zero so the reads they keep die.
	std::vector<std::pair<Inst*, size_t>>           dropped;
	// v_interp_mov P0 reads replaced by a flat input of the raw dword (attribute index).
	std::unordered_map<Inst*, int32_t>              flat_reads;
	std::vector<Inst*>                              bary_reads;
	uint32_t                                        exec_merge_assumptions = 0;
	std::string                                     reason;

private:
	bool Reject(std::string text) {
		if (reason.empty()) reason = std::move(text);
		return false;
	}

	Abs Get(const Value& value) const {
		Abs result;
		if (value.IsImmediate()) {
			result.kind = Kind::Const;
			switch (value.GetType()) {
				case Type::F32: result.bits = AsBits(value.F32Value()); break;
				case Type::U32: result.bits = value.U32(); break;
				case Type::F16: result.bits = value.F16Bits(); break;
				case Type::U16: result.bits = value.U16(); break;
				default: result.kind = Kind::Independent; break;
			}
			return result;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return result;
		const auto found = values.find(inst);
		return found != values.end() ? found->second : result;
	}

	Abs Operand(const Inst& inst, size_t index) {
		const auto value = inst.Arg(index);
		if (const auto* operand = value.TryInstruction();
		    operand != nullptr && inst.GetOpcode() != ValueOpcode::Phi && !visited.contains(operand)) {
			deferred.emplace_back(&inst, value);
		}
		return Get(value);
	}

	void Set(const Inst& inst, Abs value) {
		if (value.kind != Kind::Independent) values[&inst] = std::move(value);
	}

	static Abs Constant(uint32_t bits) {
		Abs result;
		result.kind = Kind::Const;
		result.bits = bits;
		return result;
	}

	int32_t AttributeIndex(const Attribute& attribute) {
		const auto found = std::ranges::find(attributes, attribute);
		if (found != attributes.end()) return static_cast<int32_t>(found - attributes.begin());
		attributes.push_back(attribute);
		attribute_bary.push_back(-1);
		return static_cast<int32_t>(attributes.size() - 1);
	}

	// A vertex value converted to float: a leaf that later reads the interpolated attribute.
	Abs MakeLeaf(Inst& inst, const Attribute& attribute, uint32_t vertex, double coefficient,
	         bool difference) {
		Abs result;
		result.kind      = Kind::Form;
		result.attribute = AttributeIndex(attribute);
		if (difference) {
			result.c[0][0] = -coefficient;
			result.c[vertex][0] += coefficient;
		} else {
			result.c[vertex][0] = coefficient;
		}
		leaves[&inst] = {result.attribute, difference ? 0.0 : coefficient};
		return result;
	}

	// value == M * ((1 - I - J) x0 + I x1 + J x2) for some M.
	static bool Proportional(const Abs& value) {
		if (value.kind != Kind::Form) return false;
		double scale = 0.0;
		for (const auto& row: value.c)
			for (const auto coefficient: row) scale = std::max(scale, std::abs(coefficient));
		const auto tolerance = scale * 1e-5 + 1e-30;
		const auto m         = value.c[1][1];
		const auto near      = [&](double a, double b) { return std::abs(a - b) <= tolerance; };
		return near(value.c[0][0], m) && near(value.c[0][1], -m) && near(value.c[0][2], -m) &&
		       near(value.c[1][0], 0) && near(value.c[1][2], 0) && near(value.c[2][0], 0) &&
		       near(value.c[2][1], 0) && near(value.c[2][2], m);
	}

	// Any use not modelled: vertex values must be complete interpolations here.
	void Sink(const Inst& inst, const Abs& value) {
		switch (value.kind) {
			case Kind::Raw:
			case Kind::Field:
				// The first vertex's dword used as is (an ID, a mask) is what a flat input
				// supplies when the first vertex provokes.
				if (value.vertex == 0 && !value.difference && value.source != nullptr &&
				    pixel.parameter_mode == ShaderPixelParameterMode::FirstVertex) {
					flat_reads[const_cast<Inst*>(value.source)] = AttributeIndex(
					    {value.param, value.chan, 0, 32, PackedVaryingFormat::Float32, true});
					return;
				}
				Reject(fmt::format("raw vertex value used by {}", ValueOpcodeName(inst.GetOpcode())));
				return;
			case Kind::Bary:
			case Kind::Carried:
				Reject(fmt::format("{} used by {}", value.kind == Kind::Bary ? "I/J" : "carried value",
				                   ValueOpcodeName(inst.GetOpcode())));
				return;
			case Kind::Form: break;
			default: return;
		}
		if (!Proportional(value)) {
			Reject(fmt::format("partial interpolation used by {} (c0={},{},{} c1={},{},{} c2={},{},{})",
			                   ValueOpcodeName(inst.GetOpcode()), value.c[0][0], value.c[0][1], value.c[0][2],
			                   value.c[1][0], value.c[1][1], value.c[1][2], value.c[2][0],
			                   value.c[2][1], value.c[2][2]));
			return;
		}
		double scale = 0.0;
		for (const auto& row: value.c)
			for (const auto coefficient: row) scale = std::max(scale, std::abs(coefficient));
		const auto m = value.c[1][1];
		const auto near = [&](double a, double b) { return std::abs(a - b) <= scale * 1e-5 + 1e-30; };
		if (near(m, 0)) return;
		auto& bary = attribute_bary[value.attribute];
		if (bary >= 0 && bary != value.bary) {
			Reject("one field interpolated with two barycentric kinds");
			return;
		}
		bary = value.bary;
	}

	static bool SameFactors(const Abs& a, const Abs& b) { return a.factors == b.factors; }

	static int32_t MergeBary(int32_t a, int32_t b) { return a < 0 ? b : b < 0 ? a : (a == b ? a : -2); }

	static void Scale(Abs& value, double k) {
		for (auto& row: value.c)
			for (auto& coefficient: row) coefficient *= k;
	}

	static bool Degree0(const Abs& value) {
		return std::ranges::all_of(value.c, [](const Affine& row) { return row[1] == 0 && row[2] == 0; });
	}

	// a + sign * b with at least one side per-vertex.
	Abs Add(const Inst& inst, const Abs& a, const Abs& b, double sign) {
		const auto plain = [](const Abs& value) { return !value.PerVertex(); };
		if (a.kind == Kind::Form && plain(b)) return a;
		if (b.kind == Kind::Form && plain(a)) {
			auto result = b;
			Scale(result, sign);
			return result;
		}
		if (a.kind == Kind::Bary && b.kind == Kind::Const && a.factors.empty()) {
			auto result = a;
			result.c[0][0] += sign * AsFloat(b.bits);
			return result;
		}
		if (b.kind == Kind::Bary && a.kind == Kind::Const && b.factors.empty()) {
			auto result = b;
			Scale(result, sign);
			result.c[0][0] += AsFloat(a.bits);
			return result;
		}
		if (a.kind == b.kind && (a.kind == Kind::Form || a.kind == Kind::Bary) &&
		    a.attribute == b.attribute && SameFactors(a, b)) {
			auto       result = a;
			const auto bary   = MergeBary(a.bary, b.bary);
			if (bary == -2) {
				Reject("two barycentric kinds in one sum");
				return {};
			}
			result.bary = bary;
			for (size_t k = 0; k < 3; k++)
				for (size_t t = 0; t < 3; t++) result.c[k][t] += sign * b.c[k][t];
			return result;
		}
		Sink(inst, a);
		Sink(inst, b);
		if (reason.empty() && (a.kind == Kind::Bary || b.kind == Kind::Bary)) {
			Reject("I/J added to an untracked value");
		}
		return {};
	}

	// The instruction a factor stands for, through bit casts.
	static const Inst* FactorOf(const Value& value) {
		const auto* inst = value.TryInstruction();
		while (inst != nullptr && (inst->GetOpcode() == ValueOpcode::BitCastF32U32 ||
		                           inst->GetOpcode() == ValueOpcode::BitCastU32F32 ||
		                           inst->GetOpcode() == ValueOpcode::Identity)) {
			const auto* source = inst->Arg(0).TryInstruction();
			if (source == nullptr) break;
			inst = source;
		}
		return inst;
	}

	// a * b; av and bv are the operands the values came from.
	Abs Multiply(const Inst& inst, const Abs& a, const Value& av, const Abs& b, const Value& bv) {
		if (!a.PerVertex() && !b.PerVertex()) return {};
		if (b.PerVertex() && !a.PerVertex()) return Multiply(inst, b, bv, a, av);
		// a is per-vertex.
		if (a.kind != Kind::Form && a.kind != Kind::Bary) {
			Sink(inst, a);
			return {};
		}
		if (b.kind == Kind::Const) {
			auto result = a;
			Scale(result, AsFloat(b.bits));
			return result;
		}
		if (!b.PerVertex()) {
			const auto* factor = FactorOf(bv);
			if (factor == nullptr) {
				Reject("untracked factor");
				return {};
			}
			auto result = a;
			result.factors.insert(std::ranges::upper_bound(result.factors, factor), factor);
			return result;
		}
		if (a.kind == Kind::Bary && b.kind == Kind::Form) return Multiply(inst, b, bv, a, av);
		if (a.kind == Kind::Form && b.kind == Kind::Bary) {
			if (!Degree0(a)) {
				Reject("vertex value times I/J twice");
				return {};
			}
			const auto bary = MergeBary(a.bary, b.bary);
			if (bary == -2) {
				Reject("two barycentric kinds in one product");
				return {};
			}
			auto result = a;
			result.bary = bary;
			for (const auto* factor: b.factors)
				result.factors.insert(std::ranges::upper_bound(result.factors, factor), factor);
			for (size_t k = 0; k < 3; k++) {
				const auto base = a.c[k][0];
				for (size_t t = 0; t < 3; t++) result.c[k][t] = base * b.c[0][t];
			}
			return result;
		}
		// Two complete interpolations (or anything else per-vertex): both are sinks.
		Sink(inst, a);
		Sink(inst, b);
		return {};
	}

	Abs EvaluateConstant(const Inst& inst, std::span<const Abs> args) {
		const auto op = inst.GetOpcode();
		const auto f  = [&](size_t i) { return AsFloat(args[i].bits); };
		switch (op) {
			case ValueOpcode::BitCastF32U32:
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::Identity: return Constant(args[0].bits);
			case ValueOpcode::BitCastU16F16:
			case ValueOpcode::BitCastF16U16:
			case ValueOpcode::ConvertU16U32:
			case ValueOpcode::ConvertU32U16: return Constant(args[0].bits & 0xffffu);
			case ValueOpcode::ConvertF16F32: {
				const auto half = FloatToHalfExact(f(0));
				return half ? Constant(*half) : Abs {};
			}
			case ValueOpcode::ConvertF32F16: return Constant(AsBits(HalfToFloat(args[0].bits & 0xffffu)));
			case ValueOpcode::BitFieldUExtract: {
				const auto offset = args[1].bits, count = args[2].bits;
				if (offset >= 32 || count == 0 || count > 32 - offset) return {};
				const auto mask = count == 32 ? ~0u : (1u << count) - 1u;
				return Constant((args[0].bits >> offset) & mask);
			}
			case ValueOpcode::ConvertF32U32: return Constant(AsBits(static_cast<float>(args[0].bits)));
			case ValueOpcode::ConvertF32S32:
				return Constant(AsBits(static_cast<float>(static_cast<int32_t>(args[0].bits))));
			case ValueOpcode::FPNeg32: return Constant(args[0].bits ^ 0x80000000u);
			case ValueOpcode::FPMul32: return Constant(AsBits(f(0) * f(1)));
			case ValueOpcode::FPAdd32: return Constant(AsBits(f(0) + f(1)));
			case ValueOpcode::FPSub32: return Constant(AsBits(f(0) - f(1)));
			default: return {};
		}
	}

	bool SlotHasSmoothAlias(uint32_t param) const {
		const auto location = ShaderPixelParameterMappedLocation(pixel, param);
		for (uint32_t alias = 0; alias < pixel.input_num; alias++) {
			if (ShaderPixelParameterMappedLocation(pixel, alias) == location &&
			    !ShaderPixelParameterIsFlat(pixel, alias)) {
				return true;
			}
		}
		return false;
	}

	void VisitInterpolationParameter(Inst& inst) {
		if (!inst.Arg(0).IsImmediate() || !inst.Arg(1).IsImmediate() || !inst.Arg(2).IsImmediate()) {
			Reject("dynamic v_interp_mov");
			return;
		}
		const auto param = inst.Arg(0).U32(), chan = inst.Arg(1).U32(), mode = inst.Arg(2).U32();
		if (param >= pixel.input_num || chan >= 4 || mode >= 3) {
			Reject("v_interp_mov out of range");
			return;
		}
		if (ShaderPixelParameterIsFlat(pixel, param)) {
			// A native flat input. When its slot also has a smooth alias, one native variable cannot
			// serve both modes; with single_mode_alias_slots the flat read becomes a flat packed
			// varying of the same exported dword (the provoking vertex's, as the native input).
			if (program.single_mode_alias_slots && SlotHasSmoothAlias(param)) {
				flat_reads[&inst] = AttributeIndex({param, chan, 0, 32, PackedVaryingFormat::Float32, true});
			}
			return;
		}
		Abs result;
		result.kind       = Kind::Raw;
		result.source     = &inst;
		result.param      = param;
		result.chan       = chan;
		result.vertex     = (mode + 1u) % 3u;
		result.difference = !ShaderPixelParameterIsCustom(pixel, param) && mode < 2u;
		Set(inst, result);
	}

	void VisitSelect(Inst& inst) {
		const auto condition = Operand(inst, 0);
		const auto taken = Operand(inst, 1), kept = Operand(inst, 2);
		if (condition.PerVertex()) Sink(inst, condition);
		if (inst.Flags<SelectOrigin>() == SelectOrigin::ExecMerge) {
			// The lanes the write skipped keep the register's previous value; they are not
			// expected to read it again (see the header).
			if (kept.PerVertex()) {
				exec_merge_assumptions++;
				if (drop_carried && !Proportional(kept)) dropped.emplace_back(&inst, 2);
			}
			Set(inst, taken);
			return;
		}
		if (taken.kind == Kind::Const && kept.kind == Kind::Const && taken.bits == kept.bits) {
			Set(inst, taken);
			return;
		}
		Sink(inst, taken);
		Sink(inst, kept);
	}

	void VisitPhi(const Inst& inst) {
		std::vector<Abs> args;
		bool pending = false;
		for (size_t i = 0; i < inst.NumArgs(); i++) {
			const auto value = inst.Arg(i);
			if (const auto* operand = value.TryInstruction();
			    operand != nullptr && !visited.contains(operand)) {
				deferred.emplace_back(&inst, value);
				pending = true;
				continue;
			}
			args.push_back(Get(value));
		}
		const bool per_vertex = std::ranges::any_of(args, [](const Abs& a) { return a.PerVertex(); });
		if (!per_vertex) return;
		if (pending) {
			Reject("vertex data in a loop phi");
			return;
		}
		const auto& first = args.front();
		const bool same = std::ranges::all_of(args, [&](const Abs& a) {
			return a.kind == first.kind && a.attribute == first.attribute && a.bary == first.bary &&
			       a.factors == first.factors && a.c == first.c && a.param == first.param &&
			       a.chan == first.chan && a.vertex == first.vertex && a.difference == first.difference &&
			       a.offset == first.offset && a.count == first.count &&
			       a.is_signed == first.is_signed && a.negate == first.negate;
		});
		if (same && first.kind != Kind::Bary) {
			Set(inst, first);
			return;
		}
		// Different complete interpolations: an ordinary value from here on.
		if (std::ranges::all_of(args, [](const Abs& a) { return !a.PerVertex() || Proportional(a); })) {
			for (const auto& a: args) Sink(inst, a);
			return;
		}
		// A register that keeps a partial term, raw value or I/J on one path: only lanes that
		// skipped the write carry it (see the header); any real use of it rejects the shader.
		Abs carried;
		carried.kind = Kind::Carried;
		Set(inst, carried);
	}

	void Visit(Inst& inst) {
		const auto op = inst.GetOpcode();
		switch (op) {
			case ValueOpcode::GetInterpolationParameter: return VisitInterpolationParameter(inst);
			case ValueOpcode::GetBuiltin:
				if (inst.Arg(0).IsImmediate() && IsBaryKind(inst.Arg(0).U32()) &&
				    inst.Arg(1).IsImmediate() && inst.Arg(1).U32() < 2u && inst.HasUses()) {
					Abs result;
					result.kind = Kind::Bary;
					result.bary = static_cast<int32_t>(inst.Arg(0).U32());
					result.c[0] = inst.Arg(1).U32() == 0 ? Affine {0, 1, 0} : Affine {0, 0, 1};
					bary_reads.push_back(&inst);
					Set(inst, result);
				}
				return;
			case ValueOpcode::Phi: return VisitPhi(inst);
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectF32: return VisitSelect(inst);
			default: break;
		}
		std::vector<Abs> args;
		args.reserve(inst.NumArgs());
		for (size_t i = 0; i < inst.NumArgs(); i++) args.push_back(Operand(inst, i));
		const bool per_vertex = std::ranges::any_of(args, [](const Abs& a) { return a.PerVertex(); });
		if (!per_vertex) {
			if (!args.empty() &&
			    std::ranges::all_of(args, [](const Abs& a) { return a.kind == Kind::Const; })) {
				Set(inst, EvaluateConstant(inst, args));
			}
			return;
		}
		if (op == ValueOpcode::DppUpdateU32 && !args[2].PerVertex() && args[1].PerVertex() &&
		    (args[1].kind != Kind::Bary || drop_carried)) {
			// The kept value only survives in lanes the update does not write, like the kept
			// operand of an exec merge (I/J only with drop_carried, as DropCarriedBarycentrics).
			exec_merge_assumptions++;
			if (drop_carried) dropped.emplace_back(&inst, 1);
			if (args[0].PerVertex()) Sink(inst, args[0]);
			return;
		}
		const auto imm = [&](size_t i) { return args[i].kind == Kind::Const; };
		const auto& a  = args[0];
		switch (op) {
			case ValueOpcode::Identity:
			case ValueOpcode::BitCastU32F32: Set(inst, a); return;
			case ValueOpcode::BitCastF32U32:
				if (a.kind == Kind::Raw) {
					Set(inst, MakeLeaf(inst, {a.param, a.chan, 0, 32, PackedVaryingFormat::Float32},
					               a.vertex, 1.0, a.difference));
					return;
				}
				if (a.kind == Kind::Form || a.kind == Kind::Bary) {
					Set(inst, a);
					return;
				}
				break;
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::BitFieldSExtract:
				if (imm(1) && imm(2)) {
					const auto offset = args[1].bits, count = args[2].bits;
					if (a.kind == Kind::Raw && !a.difference && count > 0 && count < 32 &&
					    offset + count <= 32) {
						auto result      = a;
						result.kind      = Kind::Field;
						result.offset    = offset;
						result.count     = count;
						result.is_signed = op == ValueOpcode::BitFieldSExtract;
						Set(inst, result);
						return;
					}
					if (a.kind == Kind::Field && !a.is_signed && offset == 0 && count >= a.count &&
					    op == ValueOpcode::BitFieldUExtract) {
						Set(inst, a); // no-op extract of the whole field
						return;
					}
				}
				break;
			case ValueOpcode::ShiftRightLogical32:
				if (a.kind == Kind::Raw && !a.difference && imm(1) && args[1].bits > 0 &&
				    args[1].bits < 32) {
					auto result   = a;
					result.kind   = Kind::Field;
					result.offset = args[1].bits;
					result.count  = 32 - args[1].bits;
					Set(inst, result);
					return;
				}
				break;
			case ValueOpcode::BitwiseAnd32:
				if (imm(1) && std::has_single_bit(args[1].bits + 1u) && args[1].bits != 0) {
					const auto bits = static_cast<uint32_t>(std::countr_one(args[1].bits));
					if (a.kind == Kind::Raw && !a.difference) {
						auto result   = a;
						result.kind   = Kind::Field;
						result.offset = 0;
						result.count  = bits;
						Set(inst, result);
						return;
					}
					if (a.kind == Kind::Field && !a.is_signed && !a.negate && bits <= a.count) {
						auto result  = a;
						result.count = bits;
						Set(inst, result);
						return;
					}
				}
				break;
			case ValueOpcode::BitwiseXor32:
				if (a.kind == Kind::Field && !a.is_signed && a.count == 16 && imm(1) &&
				    args[1].bits == 0x8000u) {
					auto result   = a;
					result.negate = !a.negate; // flips the half float's sign
					Set(inst, result);
					return;
				}
				break;
			case ValueOpcode::ConvertU16U32:
			case ValueOpcode::BitCastF16U16:
				if (a.kind == Kind::Field && !a.is_signed && a.count == 16) {
					Set(inst, a);
					return;
				}
				break;
			case ValueOpcode::ConvertF32F16:
				if (a.kind == Kind::Field && !a.is_signed && a.count == 16) {
					Set(inst, MakeLeaf(inst, {a.param, a.chan, a.offset, 16, PackedVaryingFormat::Half},
					               a.vertex, a.negate ? -1.0 : 1.0, false));
					return;
				}
				break;
			case ValueOpcode::ConvertF32U32:
				if (a.kind == Kind::Field && !a.is_signed && !a.negate) {
					Set(inst, MakeLeaf(inst, {a.param, a.chan, a.offset, a.count, PackedVaryingFormat::Unsigned},
					               a.vertex, 1.0, false));
					return;
				}
				break;
			case ValueOpcode::ConvertF32S32:
				if (a.kind == Kind::Field && a.is_signed && !a.negate) {
					Set(inst, MakeLeaf(inst, {a.param, a.chan, a.offset, a.count, PackedVaryingFormat::Signed},
					               a.vertex, 1.0, false));
					return;
				}
				break;
			case ValueOpcode::FPNeg32:
				if (a.kind == Kind::Form || a.kind == Kind::Bary) {
					auto result = a;
					Scale(result, -1.0);
					Set(inst, result);
					return;
				}
				break;
			case ValueOpcode::FPAdd32: Set(inst, Add(inst, args[0], args[1], 1.0)); return;
			case ValueOpcode::FPSub32: Set(inst, Add(inst, args[0], args[1], -1.0)); return;
			case ValueOpcode::FPMul32:
				Set(inst, Multiply(inst, args[0], inst.Arg(0), args[1], inst.Arg(1)));
				return;
			case ValueOpcode::FPFma32:
			case ValueOpcode::FPMad32: {
				const auto product = Multiply(inst, args[0], inst.Arg(0), args[1], inst.Arg(1));
				if (!reason.empty()) return;
				Set(inst, Add(inst, product, args[2], 1.0));
				return;
			}
			case ValueOpcode::FPMax32: {
				// max(k, c * x) with k = c * -(2^(n-1) - 1) on a signed n-bit field of one vertex is
				// the snorm clamp, which the previous stage applies to the field itself.
				const auto& form  = args[0].kind == Kind::Form ? args[0] : args[1];
				const auto& bound = args[0].kind == Kind::Form ? args[1] : args[0];
				if (form.kind == Kind::Form && bound.kind == Kind::Const && form.factors.empty() &&
				    Degree0(form) &&
				    attributes[form.attribute].format == PackedVaryingFormat::Signed) {
					uint32_t vertices = 0;
					double   coefficient = 0;
					for (const auto& row: form.c) {
						if (row[0] != 0) {
							vertices++;
							coefficient = row[0];
						}
					}
					const auto limit = -(std::ldexp(1.0, static_cast<int>(
					                         attributes[form.attribute].count) - 1) - 1.0);
					const auto k = static_cast<double>(AsFloat(bound.bits));
					if (vertices == 1 && coefficient > 0 &&
					    std::abs(k - coefficient * limit) <= std::abs(coefficient * limit) * 1e-5) {
						Set(inst, form);
						return;
					}
				}
				break;
			}
			default: break;
		}
		for (const auto& arg: args) Sink(inst, arg);
	}
};

// The live instructions once every leaf reads its interpolated attribute and I/J read zero,
// mirroring EliminateDeadCode.
std::unordered_set<const Inst*> LiveAfterLowering(const Program& program, const Analyzer& analysis) {
	std::unordered_set<const Inst*> live;
	std::vector<const Inst*>        pending;
	const std::unordered_set<const Inst*> bary(analysis.bary_reads.begin(), analysis.bary_reads.end());
	const auto mark = [&](const Inst* inst) {
		if (live.insert(inst).second) pending.push_back(inst);
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.MayHaveSideEffects() || std::ranges::any_of(inst.Uses(), [](const Use& use) {
				    return use.user->Parent() == nullptr;
			    })) {
				mark(&inst);
			}
		}
	}
	while (!pending.empty()) {
		const auto* inst = pending.back();
		pending.pop_back();
		if (analysis.leaves.contains(const_cast<Inst*>(inst)) ||
		    analysis.flat_reads.contains(const_cast<Inst*>(inst))) {
			continue;
		}
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
			if (std::ranges::find(analysis.dropped, std::pair {const_cast<Inst*>(inst), arg}) !=
			    analysis.dropped.end()) {
				continue;
			}
			const auto* input = inst->Arg(arg).TryInstruction();
			if (input != nullptr && input->Parent() != nullptr && !bary.contains(input)) mark(input);
		}
	}
	return live;
}

bool IsSelect(ValueOpcode op) { return op == ValueOpcode::SelectU32 || op == ValueOpcode::SelectF32; }

} // namespace

uint32_t FoldSameConditionSelects(Program& program) {
	uint32_t folded = 0;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (!IsSelect(inst.GetOpcode())) continue;
			const auto condition = inst.Arg(0).Resolve();
			for (size_t operand = 1; operand <= 2; operand++) {
				// Each step replaces the operand by one of the inner select's operands, which SSA
				// defines earlier, so the chain ends.
				for (;;) {
					const auto* inner = inst.Arg(operand).Resolve().TryInstruction();
					if (inner == nullptr || inner->GetOpcode() != inst.GetOpcode() ||
					    !(inner->Arg(0).Resolve() == condition)) {
						break;
					}
					inst.SetArg(operand, inner->Arg(operand));
					folded++;
				}
			}
		}
	}
	return folded;
}

bool LowerManualInterpolation(Program& program, const ShaderPixelInputInfo& pixel,
                              uint32_t location_limit, bool drop_carried, std::string* reason) {
	const auto fail = [&](std::string text) {
		if (reason != nullptr) *reason = std::move(text);
		return false;
	};
	if (program.stage != ShaderType::Pixel) return fail("not a pixel shader");
	if (pixel.parameter_mode == ShaderPixelParameterMode::Rectangle) return fail("rectangle list");
	location_limit = std::min(location_limit, 32u);
	Analyzer analysis(program, pixel, drop_carried);
	if (!analysis.Run()) return fail(analysis.reason);

	// Nothing that still needs per-vertex data or I/J may stay live.
	const auto live = LiveAfterLowering(program, analysis);
	std::array<bool, 32> used {};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (!live.contains(&inst)) continue;
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::GetInterpolationParameter &&
			    !analysis.flat_reads.contains(const_cast<Inst*>(&inst))) {
				const auto param = inst.Arg(0).U32();
				if (!ShaderPixelParameterIsFlat(pixel, param)) {
					return fail(fmt::format("parameter {} still read per vertex", param));
				}
				used[param] = true;
			} else if (op == ValueOpcode::GetAttribute && inst.Arg(0).IsImmediate() &&
			           inst.Arg(0).U32() < used.size()) {
				used[inst.Arg(0).U32()] = true;
			}
		}
	}
	if (PixelParameterSlotsMixFlatAndSmooth(pixel, used, program.single_mode_alias_slots)) {
		return fail("flat and smooth aliases of one slot");
	}

	// One location per interpolation qualifier group, four fields each, from the top down.
	std::array<bool, 32> taken {};
	for (uint32_t input = 0; input < pixel.input_num; input++) {
		const auto location = ShaderPixelParameterMappedLocation(pixel, input);
		if (location < taken.size()) taken[location] = true;
	}
	std::vector<PackedVarying> varyings;
	struct Group {
		bool                       flat;
		bool                       no_perspective;
		PixelInterpolationSampling sampling;
		uint32_t                   location;
		uint32_t                   used;
	};
	std::vector<Group> groups;
	uint32_t           next = location_limit;
	for (size_t index = 0; index < analysis.attributes.size(); index++) {
		const auto& attribute = analysis.attributes[index];
		const auto  bary      = analysis.attribute_bary[index];
		PackedVarying varying {
		    .source_location  = static_cast<uint8_t>(ShaderPixelParameterMappedLocation(pixel, attribute.param)),
		    .source_component = static_cast<uint8_t>(attribute.chan),
		    .bit_offset       = static_cast<uint8_t>(attribute.offset),
		    .bit_count        = static_cast<uint8_t>(attribute.count),
		    .format           = attribute.format,
		    .flat             = attribute.flat,
		};
		switch (static_cast<StageInputKind>(attribute.flat ? -1 : bary)) {
			case StageInputKind::BaryCoordSmoothCentroid:
				varying.sampling = PixelInterpolationSampling::Centroid;
				break;
			case StageInputKind::BaryCoordSmoothSample:
				varying.sampling = PixelInterpolationSampling::Sample;
				break;
			case StageInputKind::BaryCoordNoPerspective: varying.no_perspective = true; break;
			default: break;
		}
		auto group = std::ranges::find_if(groups, [&](const Group& g) {
			return g.flat == varying.flat && g.no_perspective == varying.no_perspective &&
			       g.sampling == varying.sampling && g.used < 4;
		});
		if (group == groups.end()) {
			while (next > 0 && taken[next - 1]) next--;
			if (next == 0) return fail("no free varying location");
			next--;
			taken[next] = true;
			groups.push_back({varying.flat, varying.no_perspective, varying.sampling, next, 0});
			group = groups.end() - 1;
		}
		varying.location  = static_cast<uint8_t>(group->location);
		varying.component = static_cast<uint8_t>(group->used++);
		varyings.push_back(varying);
	}

	for (auto& [leaf, info]: analysis.leaves) {
		auto& instructions = leaf->Parent()->Instructions();
		auto  position     = std::ranges::find_if(instructions, [&](const Inst& inst) { return &inst == leaf; });
		if (info.scale == 0.0) {
			leaf->ReplaceUsesWith(Value::F32(0.0f));
			continue;
		}
		auto read = leaf->Parent()->PrependNewInst(position, ValueOpcode::GetPackedVarying,
		                                           {Value(static_cast<uint32_t>(info.attribute))});
		Value value(&*read);
		if (info.scale != 1.0) {
			value = Value(&*leaf->Parent()->PrependNewInst(
			    position, ValueOpcode::FPMul32,
			    {Value::F32(static_cast<float>(info.scale)), value}));
		}
		leaf->ReplaceUsesWith(value);
	}
	for (auto& [read, attribute]: analysis.flat_reads) {
		auto& instructions = read->Parent()->Instructions();
		auto  position = std::ranges::find_if(instructions, [&](const Inst& inst) { return &inst == read; });
		auto  flat = read->Parent()->PrependNewInst(position, ValueOpcode::GetPackedVarying,
		                                            {Value(static_cast<uint32_t>(attribute))});
		auto  bits = read->Parent()->PrependNewInst(position, ValueOpcode::BitCastU32F32,
		                                            {Value(&*flat)});
		read->ReplaceUsesWith(Value(&*bits));
	}
	for (const auto& [inst, operand]: analysis.dropped) {
		inst->SetArg(operand, inst->GetOpcode() == ValueOpcode::SelectF32 ? Value::F32(0.0f) : Value(0u));
	}
	for (auto* read: analysis.bary_reads) read->ReplaceUsesWith(Value(0u));
	RemoveIdentities(program);
	EliminateDeadCode(program.blocks);
	program.info.packed_varyings = std::move(varyings);
	if (reason != nullptr) {
		*reason = fmt::format("{} fields, {} leaves, {} flat reads, {} exec-merge assumptions",
		                      program.info.packed_varyings.size(), analysis.leaves.size(),
		                      analysis.flat_reads.size(), analysis.exec_merge_assumptions);
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
