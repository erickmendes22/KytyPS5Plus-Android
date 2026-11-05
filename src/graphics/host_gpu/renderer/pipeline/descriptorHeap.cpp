#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 8192},
    // Required by NativeDescriptorType(UniformBuffers); absent in the prior
    // Android pool and the Vulkan driver rejects even a single such set.
    // Ported from Jetsku/KytyPS5, without PC-only pool scaling on Android.
    vk::DescriptorPoolSize {vk::DescriptorType::eUniformBuffer, 4096},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 1024},
};

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
	m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
	for (const auto& pending: m_pending_pools) {
        m_master_semaphore.Wait(pending.tick);
        m_graphics.device.destroyDescriptorPool(pending.pool, nullptr);
    }
}

vk::DescriptorSet DescriptorHeap::Commit(
    vk::DescriptorSetLayout layout, const DescriptorPoolPolicy::Counts& requirements) {
    KYTY_PROFILER_FUNCTION();
    EXIT_IF(layout == nullptr);

    auto& batch = m_sets[layout];
    if (batch.size != 0) {
        return batch.sets[--batch.size];
    }
    if (Allocate(layout, batch)) {
        return batch.sets[--batch.size];
    }

    // The exhausted pool may have hundreds of unused sets of other layouts,
    // but none that fit this one. Keep it alive through the GPU's last tick.
    // Only reuse retired pools that BOTH finished on GPU and can hold the
    // requested descriptor types/counts, never a too-small old pool.
    m_pending_pools.push_back({m_current_pool, m_master_semaphore.CurrentTick(), m_current_capacities});
    const auto oldest = m_pending_pools.front().tick;
    if (!m_master_semaphore.IsFree(oldest) && oldest < m_master_semaphore.CurrentTick()) {
        m_master_semaphore.Refresh();
    }
    auto reusable = std::find_if(m_pending_pools.begin(), m_pending_pools.end(), [&](const auto& pending) {
        return DescriptorPoolPolicy::Fits(pending.capacities, requirements) &&
               m_master_semaphore.IsFree(pending.tick);
    });
    if (reusable != m_pending_pools.end()) {
        m_current_pool = reusable->pool;
        m_current_capacities = reusable->capacities;
        m_pending_pools.erase(reusable);
        EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool, {}) != vk::Result::eSuccess);
    } else {
        CreateDescriptorPool(requirements);
    }

    m_sets.clear();
    auto& fresh_batch = m_sets[layout];
    if (!Allocate(layout, fresh_batch)) {
        // A fresh, correctly dimensioned pool still failed. This must not
        // become another opaque EXIT_IF in Wolverine's diagnostics.
        EXIT("descriptor allocation still failed on fresh pool: "
             "layout=%p storage=%u uniform=%u sampled=%u storage_image=%u sampler=%u "
             "caps=%u/%u/%u/%u/%u\\n",
             static_cast<void*>(layout), requirements[0], requirements[1],
             requirements[2], requirements[3], requirements[4],
             m_current_capacities[0], m_current_capacities[1],
             m_current_capacities[2], m_current_capacities[3], m_current_capacities[4]);
    }
    return fresh_batch.sets[--fresh_batch.size];
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool = m_current_pool;
	allocate.pSetLayouts    = layouts.data();

	for (;;) {
		allocate.descriptorSetCount = batch.allocation;
		const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
		if (result == vk::Result::eSuccess) {
			batch.size = batch.allocation;
			return true;
		}
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
		if (batch.allocation == 1) {
			return false;
		}
		batch.allocation /= 2;
	}
}

void DescriptorHeap::CreateDescriptorPool(const DescriptorPoolPolicy::Counts& requirements) {
    auto sizes = DescriptorPoolSizes;
    for (size_t i = 0; i < sizes.size(); ++i) {
        sizes[i].descriptorCount = DescriptorPoolPolicy::Capacity(sizes[i].descriptorCount, requirements[i]);
        m_current_capacities[i] = sizes[i].descriptorCount;
    }

    vk::DescriptorPoolCreateInfo create {};
    create.maxSets       = DescriptorHeapCount;
    create.poolSizeCount = static_cast<uint32_t>(sizes.size());
    create.pPoolSizes    = sizes.data();
    EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool) !=
            vk::Result::eSuccess);
}

} // namespace Libs::Graphics
