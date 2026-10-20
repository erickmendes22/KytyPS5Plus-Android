#include "graphics/shader/recompiler/ir/passes/ResourceMemoRebase.h"

#include "graphics/shader/recompiler/ir/Reg.h"

#include <array>
#include <atomic>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

// Opcodes whose result is plain arithmetic on their arguments: a user-data word reaching a
// descriptor dword only through these can change that dword's value, nothing else. Every other
// opcode (memory reads, Phi, lane ops, ...) puts its arguments in address context.
bool IsPure(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32: return true;
		default: return false;
	}
}

class Classifier {
public:
	explicit Classifier(const ResourcePlan& plan): m_plan(plan) {}

	// Marks the user-data words `value` reaches; `address` once any opcode on the way is not pure.
	void Visit(Value value, bool address, std::vector<uint64_t>& words) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr) return;
		auto& seen = address ? m_seen_address : m_seen_value;
		if (!seen.insert(inst).second) return;
		if (inst->GetOpcode() == ValueOpcode::GetUserData) {
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg >= m_plan.user_data_base) Mark(words, reg - m_plan.user_data_base);
			return;
		}
		const bool args_address = address || !IsPure(inst->GetOpcode());
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
			Visit(inst->Arg(arg), args_address, args_address ? m_address_words : words);
		}
	}

	void VisitSource(uint32_t source, bool address) {
		if (source >= m_plan.descriptor_sources.size()) return;
		const auto& descriptor = m_plan.descriptor_sources[source];
		for (uint32_t dword = 0; dword < descriptor.dword_count && dword < descriptor.dwords.size(); ++dword) {
			Visit(descriptor.dwords[dword], address, address ? m_address_words : m_value_words);
		}
	}

	void ResetValueSeen() { m_seen_value.clear(); }

	static void Mark(std::vector<uint64_t>& words, size_t index) {
		if (index / 64u >= words.size()) words.resize(index / 64u + 1u, 0u);
		words[index / 64u] |= uint64_t {1} << (index % 64u);
	}

	std::vector<uint64_t> m_address_words;
	std::vector<uint64_t> m_value_words;

private:
	const ResourcePlan&             m_plan;
	std::unordered_set<const Inst*> m_seen_address;
	std::unordered_set<const Inst*> m_seen_value;
};

bool Intersects(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
	for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
		if ((a[i] & b[i]) != 0u) return true;
	}
	return false;
}

constexpr uint64_t BufferBase(const DescriptorValue& value) {
	return value.dwords[0] | (uint64_t {value.dwords[1] & 0xffffu} << 32u);
}

constexpr uint64_t ImageBase(const DescriptorValue& value) {
	return value.dwords[0] | (uint64_t {value.dwords[1] & 0xffu} << 32u);
}

// Equal except for the base address bits `high_mask` of dword 1 and all of dword 0.
bool SameExceptBase(const DescriptorValue& a, const DescriptorValue& b, uint32_t high_mask) {
	if (a.dword_count != b.dword_count || ((a.dwords[1] ^ b.dwords[1]) & ~high_mask) != 0u) return false;
	for (size_t i = 2; i < a.dwords.size(); ++i) {
		if (a.dwords[i] != b.dwords[i]) return false;
	}
	return true;
}

std::array<std::atomic<uint64_t>, static_cast<size_t>(RebaseFailure::Count)> g_failures {};

bool Fail(RebaseFailure why) {
	g_failures[static_cast<size_t>(why)].fetch_add(1, std::memory_order_relaxed);
	return false;
}

} // namespace

std::array<uint64_t, static_cast<size_t>(RebaseFailure::Count)> TakeRebaseFailures() {
	std::array<uint64_t, static_cast<size_t>(RebaseFailure::Count)> counts {};
	for (size_t i = 0; i < counts.size(); ++i) counts[i] = g_failures[i].exchange(0, std::memory_order_relaxed);
	return counts;
}

UserDataClasses ClassifyUserData(const ResourcePlan& plan) {
	UserDataClasses classes;
	// Plans that capture or strictly re-read their specialization inputs keep every word keyed.
	if (plan.capture_specialization_reads || plan.requires_specialization_memory ||
	    !plan.source_reads.empty() || !plan.resource_tracking_complete) {
		return classes;
	}
	const auto sources = static_cast<uint32_t>(plan.descriptor_sources.size());
	std::vector<uint8_t> forced(sources, 0u); // sources whose dwords count as address context
	const auto force = [&](uint32_t source) {
		if (source < sources) forced[source] = 1u;
	};
	for (uint32_t source = 0; source < sources; ++source) {
		const auto& descriptor = plan.descriptor_sources[source];
		if (!descriptor.indirect_descriptor) continue;
		force(source);
		const auto& indirect = *descriptor.indirect_descriptor;
		if (indirect.selector) {
			force(indirect.selector->source);
			if (indirect.selector->rows) force(indirect.selector->rows->source);
		}
		force(indirect.table_source);
		for (const auto candidate: indirect.sources) force(candidate);
	}
	for (const auto& buffer: plan.info.buffers) {
		if (buffer.written || buffer.atomic || buffer.image_alias != BufferResource::NoImageAlias ||
		    buffer.indirect_root != BufferResource::NoIndirectBuffer)
			force(buffer.source);
	}
	for (const auto& image: plan.info.images) {
		if (image.written || image.indirect_root != ImageResource::NoIndirectImage) force(image.source);
	}
	for (const auto& sampler: plan.info.samplers) force(sampler.source);

	Classifier classifier(plan);
	for (const auto& read: plan.srt_reads) classifier.Visit(read.value, true, classifier.m_address_words);
	for (const auto& block: plan.control_flow) classifier.Visit(block.condition, true, classifier.m_address_words);
	for (const auto& value: plan.uniform_fill.values) classifier.Visit(value, true, classifier.m_address_words);
	for (uint32_t source = 0; source < sources; ++source) {
		const auto& descriptor = plan.descriptor_sources[source];
		if (descriptor.indirect_descriptor) {
			const auto& indirect = *descriptor.indirect_descriptor;
			for (const auto& value: {indirect.key_count, indirect.selector_first, indirect.selector_mask}) {
				classifier.Visit(value, true, classifier.m_address_words);
			}
		}
		if (forced[source]) classifier.VisitSource(source, true);
	}
	for (uint32_t source = 0; source < sources; ++source) {
		if (!forced[source]) classifier.VisitSource(source, false);
	}
	classes.value_words = classifier.m_value_words;
	for (size_t i = 0; i < classes.value_words.size(); ++i) {
		if (i < classifier.m_address_words.size()) classes.value_words[i] &= ~classifier.m_address_words[i];
	}
	// Per source: does it use a value word?
	for (uint32_t source = 0; source < sources; ++source) {
		if (forced[source]) continue;
		Classifier one(plan);
		one.VisitSource(source, false);
		if (Intersects(one.m_value_words, classes.value_words)) classes.value_sources.push_back(source);
	}
	return classes;
}

bool EvaluateValueSources(const ResourcePlan& plan, const SrtRuntime& runtime,
                          std::span<const uint32_t> sources, std::span<const uint8_t> active,
                          std::vector<DescriptorValue>& values) {
	values.assign(sources.size(), {});
	SrtWalker walker(plan, runtime);
	for (size_t i = 0; i < sources.size(); ++i) {
		const auto source = sources[i];
		if (!active.empty() && (source >= active.size() || !active[source])) continue;
		if (!walker.EvaluateDescriptor(source, values[i])) return false;
	}
	return true;
}

bool RebaseSnapshot(const ResourcePlan& plan, std::span<const uint32_t> sources,
                    std::span<const DescriptorValue> cached, std::span<const DescriptorValue> current,
                    ResourceSnapshot& snapshot) {
	for (size_t k = 0; k < sources.size(); ++k) {
		if (cached[k] == current[k]) continue;
		const auto source = sources[k];
		for (size_t i = 0; i < plan.info.buffers.size() && i < snapshot.buffers.size(); ++i) {
			if (plan.info.buffers[i].source != source) continue;
			auto& kept = snapshot.buffers[i];
			if (kept != cached[k]) return Fail(RebaseFailure::Normalized);
			if (!SameExceptBase(cached[k], current[k], 0xffffu)) return Fail(RebaseFailure::NotBaseOnly);
			if ((BufferBase(cached[k]) == 0u) != (BufferBase(current[k]) == 0u)) return Fail(RebaseFailure::Null);
			// The only base-dependent specialization field of a buffer: uniform (16-byte alignment).
			const auto index = static_cast<uint32_t>(i);
			if (BufferSpecializedAsUniform(plan, index, cached[k]) != BufferSpecializedAsUniform(plan, index, current[k]))
				return Fail(RebaseFailure::Uniform);
			kept = current[k];
		}
		for (size_t i = 0; i < plan.info.images.size() && i < snapshot.images.size(); ++i) {
			if (plan.info.images[i].source != source) continue;
			auto& kept = snapshot.images[i];
			if (kept != cached[k]) return Fail(RebaseFailure::Normalized);
			if (!SameExceptBase(cached[k], current[k], 0xffu)) return Fail(RebaseFailure::NotBaseOnly);
			if ((ImageBase(cached[k]) == 0u) != (ImageBase(current[k]) == 0u)) return Fail(RebaseFailure::Null);
			kept = current[k];
		}
		for (const auto& sampler: plan.info.samplers) {
			if (sampler.source == source) return Fail(RebaseFailure::Sampler);
		}
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
