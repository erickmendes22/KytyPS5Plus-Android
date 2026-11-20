// Ported from Jetsku/KytyPS5 U59 (GPL-2.0), descriptor-set exact-content reuse.
#include "graphics/host_gpu/renderer/pipeline/descriptorSetReuse.h"

#include "common/assert.h"

#include <xxhash.h>

namespace Libs::Graphics {

namespace {

[[nodiscard]] bool IsBufferDescriptor(vk::DescriptorType type) {
	return type == vk::DescriptorType::eStorageBuffer || type == vk::DescriptorType::eUniformBuffer;
}

[[nodiscard]] bool IsImageDescriptor(vk::DescriptorType type) {
	return type == vk::DescriptorType::eSampler || type == vk::DescriptorType::eSampledImage ||
	       type == vk::DescriptorType::eStorageImage ||
	       type == vk::DescriptorType::eCombinedImageSampler;
}

// Only plain buffer/image/sampler writes are cached: no extension structures, texel buffer
// views or other descriptor types (the renderer never writes them).
[[nodiscard]] bool Supported(const vk::WriteDescriptorSet& write) {
	return write.pNext == nullptr && write.pTexelBufferView == nullptr &&
	       ((IsBufferDescriptor(write.descriptorType) && write.pBufferInfo != nullptr) ||
	        (IsImageDescriptor(write.descriptorType) && write.pImageInfo != nullptr));
}

void Mix(uint64_t& hash, uint64_t value) {
	hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
}

template <typename T>
[[nodiscard]] uint64_t HandleBits(T handle) {
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<typename T::CType>(handle)));
}

} // namespace

uint64_t DescriptorSetReuse::Hash(vk::DescriptorSetLayout                  layout,
                                  std::span<const vk::WriteDescriptorSet> writes) {
	uint64_t hash = XXH3_64bits(&layout, sizeof(layout));
	for (const auto& write: writes) {
		Mix(hash, (static_cast<uint64_t>(write.dstBinding) << 32u) ^ write.descriptorCount);
		Mix(hash, (static_cast<uint64_t>(write.descriptorType) << 32u) ^ write.dstArrayElement);
		for (uint32_t i = 0; i < write.descriptorCount; i++) {
			if (IsBufferDescriptor(write.descriptorType) && write.pBufferInfo != nullptr) {
				const auto& info = write.pBufferInfo[i];
				Mix(hash, HandleBits(info.buffer));
				Mix(hash, info.offset);
				Mix(hash, info.range);
			} else if (IsImageDescriptor(write.descriptorType) && write.pImageInfo != nullptr) {
				const auto& info = write.pImageInfo[i];
				Mix(hash, HandleBits(info.sampler));
				Mix(hash, HandleBits(info.imageView));
				Mix(hash, static_cast<uint64_t>(info.imageLayout));
			}
		}
	}
	return hash;
}

bool DescriptorSetReuse::Matches(const Entry& entry, uint64_t tick, vk::DescriptorSetLayout layout,
                                 std::span<const vk::WriteDescriptorSet> writes, uint64_t hash) {
	if (entry.set == nullptr || entry.tick != tick || entry.hash != hash ||
	    entry.layout != layout || entry.writes.size() != writes.size()) {
		return false;
	}
	size_t buffer_index = 0;
	size_t image_index  = 0;
	for (size_t i = 0; i < writes.size(); i++) {
		const auto& write = writes[i];
		const auto& old   = entry.writes[i];
		if (!Supported(write) || write.dstBinding != old.dstBinding ||
		    write.dstArrayElement != old.dstArrayElement ||
		    write.descriptorCount != old.descriptorCount ||
		    write.descriptorType != old.descriptorType) {
			return false;
		}
		for (uint32_t j = 0; j < write.descriptorCount; j++) {
			if (IsBufferDescriptor(write.descriptorType)) {
				if (buffer_index >= entry.buffers.size() ||
				    !(write.pBufferInfo[j] == entry.buffers[buffer_index++])) {
					return false;
				}
			} else if (image_index >= entry.images.size() ||
			           !(write.pImageInfo[j] == entry.images[image_index++])) {
				return false;
			}
		}
	}
	return buffer_index == entry.buffers.size() && image_index == entry.images.size();
}

vk::DescriptorSet DescriptorSetReuse::Find(uint64_t tick, vk::DescriptorSetLayout layout,
                                           std::span<const vk::WriteDescriptorSet> writes,
                                           uint64_t hash) const {
	const auto& entry = m_entries[hash % Slots];
	return Matches(entry, tick, layout, writes, hash) ? entry.set : vk::DescriptorSet {};
}

uint64_t DescriptorSetReuse::Digest(vk::DescriptorSetLayout                  layout,
                                    std::span<const vk::WriteDescriptorSet> writes) {
	thread_local std::vector<uint64_t> words;
	words.clear();
	words.push_back(HandleBits(layout));
	for (const auto& write: writes) {
		words.push_back((static_cast<uint64_t>(write.dstBinding) << 32u) | write.descriptorCount);
		words.push_back((static_cast<uint64_t>(write.descriptorType) << 32u) |
		                write.dstArrayElement);
		for (uint32_t i = 0; i < write.descriptorCount; i++) {
			if (IsBufferDescriptor(write.descriptorType) && write.pBufferInfo != nullptr) {
				const auto& info = write.pBufferInfo[i];
				words.push_back(HandleBits(info.buffer));
				words.push_back(info.offset);
				words.push_back(info.range);
			} else if (IsImageDescriptor(write.descriptorType) && write.pImageInfo != nullptr) {
				const auto& info = write.pImageInfo[i];
				words.push_back(HandleBits(info.sampler));
				words.push_back(HandleBits(info.imageView));
				words.push_back(static_cast<uint64_t>(info.imageLayout));
			}
		}
	}
	return XXH3_64bits(words.data(), words.size() * sizeof(uint64_t));
}

DescriptorSetReuse::AuditResult DescriptorSetReuse::Audit(uint64_t tick, uint64_t digest) {
	if (tick != m_audit_tick) {
		m_audit_tick = tick;
		m_audit_seen.clear();
		m_audit_slot_used.fill(false);
	}
	AuditResult result;
	result.repeat          = !m_audit_seen.insert(digest).second;
	const auto slot        = digest % Slots;
	result.digest_slot_hit = m_audit_slot_used[slot] && m_audit_slots[slot] == digest;
	// As Insert does on a miss: the slot takes the newest set.
	m_audit_slots[slot]     = digest;
	m_audit_slot_used[slot] = true;
	return result;
}

void DescriptorSetReuse::Insert(uint64_t tick, vk::DescriptorSetLayout layout,
                                std::span<const vk::WriteDescriptorSet> writes, uint64_t hash,
                                vk::DescriptorSet set) {
	auto& entry = m_entries[hash % Slots];
	entry.set   = nullptr;
	for (const auto& write: writes) {
		if (!Supported(write)) {
			return;
		}
	}
	entry.writes.assign(writes.begin(), writes.end());
	entry.buffers.clear();
	entry.images.clear();
	for (auto& write: entry.writes) {
		if (IsBufferDescriptor(write.descriptorType)) {
			entry.buffers.insert(entry.buffers.end(), write.pBufferInfo,
			                     write.pBufferInfo + write.descriptorCount);
		} else {
			entry.images.insert(entry.images.end(), write.pImageInfo,
			                    write.pImageInfo + write.descriptorCount);
		}
		write.pBufferInfo = nullptr;
		write.pImageInfo  = nullptr;
		write.dstSet      = nullptr;
	}
	entry.tick   = tick;
	entry.hash   = hash;
	entry.layout = layout;
	entry.set    = set;
}

} // namespace Libs::Graphics
