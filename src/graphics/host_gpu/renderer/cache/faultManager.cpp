#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/vulkanDebugLabel.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults    = 1024;
constexpr size_t PageFaultAreaSize = MaxPageFaults * sizeof(uint64_t);

} // namespace

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                        MaxPendingFaults * PageFaultAreaSize) {
	SetVulkanObjectNameF(m_graphics.device, m_fault_buffer.Handle(), "Fault Buffer");

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                &m_fault_process_desc_layout),
	    "create fault-buffer descriptor layout");

	const auto module = CompileSPV(FAULT_BUFFER_PROCESS_SPV, m_graphics.device);

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts    = &m_fault_process_desc_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                           &m_fault_process_pipeline_layout),
	    "create fault-buffer pipeline layout");

	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_fault_process_pipeline_layout;
	const auto result = m_graphics.device.createComputePipelines(
	    nullptr, 1, &pipeline_info, nullptr, &m_fault_process_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create fault-buffer pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_fault_process_pipeline, "Fault Buffer Parser");
}

FaultManager::~FaultManager() {
	m_graphics.device.destroyPipeline(m_fault_process_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_fault_process_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_fault_process_desc_layout, nullptr);
}

void FaultManager::ProcessFaultBuffer() {
	if (const auto wait_tick = m_fault_areas[m_current_area]; wait_tick != 0) {
		const Common::PerfStats::ScopedWaitSite wait_site(Common::PerfStats::Wait::SemFaultBuffer);
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}

	const auto offset = m_current_area * PageFaultAreaSize;
	auto*      mapped = m_download_buffer.Mapped().data() + offset;
	std::memset(mapped, 0, PageFaultAreaSize);
	m_download_buffer.Flush(offset, PageFaultAreaSize);

	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset        = 0;
	pre_barrier.size           = m_fault_buffer.Size();
	auto post_barrier         = pre_barrier;
	post_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	post_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	post_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), 0, BufferCache::FAULT_BITMAP_WORDS * sizeof(uint32_t)},
	    {m_download_buffer.Handle(), offset, PageFaultAreaSize},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}

	m_scheduler.EndRendering(PassEnd::Buffer);
	auto command = m_scheduler.Current().Handle();
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &pre_barrier;
	command.pipelineBarrier2(dependency);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
	                             m_fault_process_pipeline_layout, 0, writes);
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 32;
	const auto num_workgroups = (num_threads + 63) / 64;
	// Marked so GPU hang dumps show this dispatch instead of folding it into "other".
	m_scheduler.Current().MarkGpu("misc");
	{
		const ScopedDebugLabel label(command, "fault-scan", 0);
		command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	}
	m_scheduler.Current().MarkGpu("fault-scan");
	dependency.pBufferMemoryBarriers = &post_barrier;
	command.pipelineBarrier2(dependency);

	const auto area = m_current_area;
	m_scheduler.DeferOperation([this, mapped, offset, area] {
		m_download_buffer.Invalidate(offset, PageFaultAreaSize);
		RangeSet    fault_ranges;
		const auto* faults = std::bit_cast<const uint64_t*>(mapped);
		// The first word counts attempted reservations, including those that did not fit.
		const auto  count  = std::min(static_cast<uint32_t>(faults[0]), uint32_t {MaxPageFaults - 1});
		for (uint32_t index = 1; index <= count; ++index) {
			const auto address = BufferCache::GuestAddress(faults[index]);
			fault_ranges.Add(address, BufferCache::CACHING_PAGESIZE);
			LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", address);
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			(void)m_buffer_cache.FindBuffer(start, end - start);
		});
		m_fault_areas[area] = 0;
	});

	m_fault_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingFaults;
}

void FaultManager::PrepareBdaWrites() {
	if (!m_bda_initialized) {
		constexpr uint64_t begin = BufferCache::BDA_WRITE_BITMAP_WORD * sizeof(uint32_t);
		m_fault_buffer.Fill(begin, BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE - begin, 0);
		m_bda_initialized = true;
	}
}

FaultManager::BdaWrites FaultManager::CollectBdaWrites() {
	PrepareBdaWrites();
	// The existing GPU fault-scanner also compacts the separate GPU-written bitmap.
	// Its source descriptor is restricted to this second bitmap, leaving read faults intact.
	constexpr uint64_t begin = BufferCache::BDA_WRITE_BITMAP_WORD * sizeof(uint32_t);
	constexpr uint64_t bytes = BufferCache::FAULT_BITMAP_WORDS * sizeof(uint32_t);
	constexpr uint64_t counter = BufferCache::BDA_DROPPED_WRITES_WORD * sizeof(uint32_t);
	if (!m_bda_download) {
		m_bda_download = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Download, 0, AllFlags,
		    PageFaultAreaSize + sizeof(uint32_t));
	}
	auto* mapped = m_bda_download->Mapped().data();
	std::memset(mapped, 0, sizeof(uint64_t));
	m_bda_download->Flush(0, sizeof(uint64_t));
	m_scheduler.EndRendering(PassEnd::Buffer);
	const auto command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier2 before {};
	before.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	before.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
	                      vk::PipelineStageFlagBits2::eTransfer;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead |
	                       vk::AccessFlagBits2::eShaderWrite |
	                       vk::AccessFlagBits2::eTransferRead;
	before.buffer = m_fault_buffer.Handle();
	before.offset = begin;
	before.size = BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE - begin;
	vk::DependencyInfo dep {};
	dep.bufferMemoryBarrierCount = 1;
	dep.pBufferMemoryBarriers = &before;
	command.pipelineBarrier2(dep);
	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), begin, bytes},
	    {m_bda_download->Handle(), 0, PageFaultAreaSize},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo = &infos[i];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
	                             m_fault_process_pipeline_layout, 0, writes);
	constexpr uint64_t threads = BufferCache::FAULT_BITMAP_WORDS;
	command.dispatch(static_cast<uint32_t>((threads + 63u) / 64u), 1, 1);
	vk::BufferMemoryBarrier2 after = before;
	after.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
	after.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	dep.pBufferMemoryBarriers = &after;
	command.pipelineBarrier2(dep);
	const vk::BufferCopy dropped_copy {counter, PageFaultAreaSize, sizeof(uint32_t)};
	command.copyBuffer(m_fault_buffer.Handle(), m_bda_download->Handle(), 1, &dropped_copy);
	// Ensure the compacted page list and dropped-write counter are host visible.
	vk::BufferMemoryBarrier2 host {};
	host.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader |
	                    vk::PipelineStageFlagBits2::eTransfer;
	host.srcAccessMask = vk::AccessFlagBits2::eShaderWrite |
	                     vk::AccessFlagBits2::eTransferWrite;
	host.dstStageMask = vk::PipelineStageFlagBits2::eHost;
	host.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	host.buffer = m_bda_download->Handle();
	host.offset = 0;
	host.size = PageFaultAreaSize + sizeof(uint32_t);
	dep.pBufferMemoryBarriers = &host;
	command.pipelineBarrier2(dep);
	// Wait on the actual producer/scan GPU tick, not a CPU approximation.
	m_scheduler.Wait(m_scheduler.CurrentTick());
	m_bda_download->Invalidate(0, PageFaultAreaSize + sizeof(uint32_t));
	const auto* words = reinterpret_cast<const uint64_t*>(mapped);
	const auto count = static_cast<uint32_t>(words[0]);
	BdaWrites result;
	result.overflow = count >= MaxPageFaults;
	const auto limited = std::min(count, uint32_t {MaxPageFaults - 1});
	result.pages.reserve(limited);
	for (uint32_t index = 1; index <= limited; ++index) {
		result.pages.push_back(BufferCache::GuestAddress(words[index]));
	}
	std::memcpy(&result.dropped, mapped + PageFaultAreaSize, sizeof(result.dropped));
	// Do not carry written bits into the next dispatch or confuse them with read faults.
	m_fault_buffer.Fill(begin, BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE - begin, 0);
	return result;
}

} // namespace Libs::Graphics
