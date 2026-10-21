#include "graphics/host_gpu/renderer/pipeline/ftzProbe.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/ftz32.h"

#include "gpu_blit_shaders/gpu_ftz_probe_spv.h"

#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {


struct ProbeBuffer {
	uint32_t inputs[4];
	uint32_t results[32];
};

} // namespace

uint32_t ProbeFtz32Ops(GraphicContext& graphics, bool default_on) {
	if (!Ftz32::Enabled(std::getenv("KYTY_FTZ32"), default_on)) return 0;
	const auto log = [](const std::string& text) { Log::WriteToConsoleAndLog("KYTY_FTZ32: " + text + "\n"); };

	vk::PhysicalDeviceFloatControlsProperties controls {};
	vk::PhysicalDeviceProperties2             properties {};
	properties.pNext = &controls;
	graphics.physical_device.getProperties2(&properties);
	// Native F16 code declares 16-bit DenormPreserve next to this 32-bit mode.
	const bool independent = controls.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone;
	log(fmt::format("device ftz32={} preserve32={} independence={} szinp32={}",
	                controls.shaderDenormFlushToZeroFloat32 ? 1 : 0, controls.shaderDenormPreserveFloat32 ? 1 : 0,
	                static_cast<int>(controls.denormBehaviorIndependence),
	                controls.shaderSignedZeroInfNanPreserveFloat32 ? 1 : 0));
	if (!controls.shaderDenormFlushToZeroFloat32 || !independent) {
		log("not supported: explicit flushes kept");
		return 0;
	}

	const auto device = graphics.device;
	const auto code   = Ftz32::AddDenormFlushToZero32(GPU_FTZ_PROBE_SPV);
	if (code.empty()) {
		log("probe module patch failed: explicit flushes kept");
		return 0;
	}

	// Buffer with a device address in host-visible memory.
	vk::BufferCreateInfo buffer_info {};
	buffer_info.size  = sizeof(ProbeBuffer);
	buffer_info.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;
	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
	VkBuffer          buffer     = VK_NULL_HANDLE;
	VmaAllocation     allocation = nullptr;
	VmaAllocationInfo mapped {};
	if (vmaCreateBuffer(graphics.allocator, reinterpret_cast<const VkBufferCreateInfo*>(&buffer_info),
	                    &allocation_info, &buffer, &allocation, &mapped) != VK_SUCCESS ||
	    mapped.pMappedData == nullptr) {
		log("probe buffer allocation failed: explicit flushes kept");
		return 0;
	}
	auto* data = static_cast<ProbeBuffer*>(mapped.pMappedData);
	std::memset(data, 0xcd, sizeof(ProbeBuffer));
	for (size_t i = 0; i < Ftz32::kInputs.size(); ++i) data->inputs[i] = Ftz32::kInputs[i];
	vmaFlushAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE);
	vk::BufferDeviceAddressInfo address_info {};
	address_info.buffer   = buffer;
	const uint64_t address = device.getBufferAddress(address_info);

	vk::PipelineLayout      layout   = nullptr;
	vk::Pipeline            pipeline = nullptr;
	vk::CommandPool         pool     = nullptr;
	vk::Fence               fence    = nullptr;
	uint32_t                ops      = 0;
	const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint64_t)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	bool ok = device.createPipelineLayout(&layout_info, nullptr, &layout) == vk::Result::eSuccess;
	if (ok) {
		const auto module = CompileSPV(code, device);
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
		pipeline_info.stage.module = module;
		pipeline_info.stage.pName  = "main";
		pipeline_info.layout       = layout;
		ok = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline) == vk::Result::eSuccess;
		device.destroyShaderModule(module, nullptr);
	}
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.queueFamilyIndex = graphics.queue_family;
	ok = ok && device.createCommandPool(&pool_info, nullptr, &pool) == vk::Result::eSuccess;
	vk::CommandBuffer command = nullptr;
	if (ok) {
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		ok = device.allocateCommandBuffers(&allocate, &command) == vk::Result::eSuccess;
	}
	vk::FenceCreateInfo fence_info {};
	ok = ok && device.createFence(&fence_info, nullptr, &fence) == vk::Result::eSuccess;
	if (ok) {
		vk::CommandBufferBeginInfo begin {};
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		ok = command.begin(&begin) == vk::Result::eSuccess;
	}
	if (ok) {
		command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
		command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(address), &address);
		command.dispatch(1, 1, 1);
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost, {}, 1,
		                        &barrier, 0, nullptr, 0, nullptr);
		ok = command.end() == vk::Result::eSuccess;
	}
	if (ok) {
		vk::SubmitInfo submit {};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &command;
		{
			Common::LockGuard lock(graphics.queue_mutex);
			ok = graphics.queue.submit(1, &submit, fence) == vk::Result::eSuccess;
		}
		ok = ok && device.waitForFences(1, &fence, VK_TRUE, 5'000'000'000ull) == vk::Result::eSuccess;
	}
	if (ok) {
		vmaInvalidateAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE);
		ops = Ftz32::OpsMatchingGcn(data->results);
		std::string results;
		for (uint32_t i = 0; i < Ftz32::kInputs.size(); ++i) {
			results += fmt::format(" in={:08x}:", Ftz32::kInputs[i]);
			for (uint32_t op = 0; op < Ftz32::kOpCount; ++op) {
				results += fmt::format(" {:08x}", data->results[i * Ftz32::kStride + op]);
			}
		}
		log(fmt::format("explicit flushes dropped for ops 0x{:x} (rcp=1 rsq=2 sqrt=4 exp2=8 log2=16);{}", ops,
		                results));
	} else {
		log("probe failed to run: explicit flushes kept");
	}
	if (fence) device.destroyFence(fence, nullptr);
	if (pool) device.destroyCommandPool(pool, nullptr);
	if (pipeline) device.destroyPipeline(pipeline, nullptr);
	if (layout) device.destroyPipelineLayout(layout, nullptr);
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
	return ops;
}

} // namespace Libs::Graphics
