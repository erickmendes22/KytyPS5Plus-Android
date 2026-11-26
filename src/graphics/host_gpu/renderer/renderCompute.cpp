#include "graphics/host_gpu/renderer/pipeline/demonsCountArithmeticProbe.h"
#include "graphics/host_gpu/renderer/demonsPrelightTrace.h"
#include "graphics/host_gpu/renderer/demonsIndirectWitness.h"
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include "graphics/shader/demonsPcRenderParity.h"
#include "graphics/host_gpu/renderer/demonsDominantGeometry.h"
#include "graphics/host_gpu/renderer/demonsRayCopyPolicy.h"
#include "graphics/host_gpu/renderer/demonsRaySourceHistory.h"
#include "graphics/host_gpu/renderer/demonsTransitionTrace.h"
#include "kernel/demonsFileReadHistory.h"
#include "cpu/demonsGuestWriteHistory.h"
#include "graphics/host_gpu/renderer/demonsAtlasTileHistory.h"
#include "graphics/shader/demonsPcDppPolicy.h"
#include "graphics/host_gpu/renderer/pipeline/demonsLightMathProbe.h"
#include "graphics/host_gpu/renderer/pipeline/demonsCountExecProbe.h"
#include "graphics/host_gpu/renderer/pipeline/demonsRelightProbe.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanDebugLabel.h"
#include "graphics/host_gpu/renderer/dispatchSplit.h"
#include "graphics/host_gpu/renderer/computeBufferFillPolicy.h"
#include "graphics/host_gpu/renderer/indirectDispatchPolicy.h"
#include "graphics/host_gpu/renderer/demonsRollingProbe.h"
#include "graphics/host_gpu/renderer/demonsAdaptiveCapture.h"
#include "graphics/host_gpu/renderer/htileMetaWritePolicy.h"
#include "graphics/host_gpu/renderer/demonsRadianceTrace.h"
#include "graphics/host_gpu/renderer/gpuPipeline.h"
#include "graphics/host_gpu/renderer/gpuPipelineItems.h"
#include "graphics/host_gpu/renderer/pipeline/walkFan.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/liveKnobs.h"
#include "graphics/shader/halfRateTiles.h"
#include "graphics/host_gpu/renderer/pipeline/demonsLightingMemoryPolicy.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderIndirect.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <bit>
#include <tuple>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
namespace {

struct DsBufferProducer {
	uint64_t address = 0;
	uint64_t size = 0;
	uint64_t shader = 0;
	int32_t frame = -1;
	bool indirect = false;
	bool fill = false;
	uint64_t submit = 0;
	uint64_t serial = 0;
};

// Accessed under the renderer context lock. Only the lighting trace records compute writes.
std::array<DsBufferProducer, 1024> ds_buffer_producers {};
size_t ds_next_buffer_producer = 0;
DsBufferProducer ds_light_queue_writer {};
std::array<DsBufferProducer, 64> ds_probe_buffer_writes {};
size_t ds_next_probe_write = 0;

uint64_t RememberDsBufferProducer(DsBufferProducer producer) {
	static uint64_t serial = 0;
	producer.serial = ++serial;
	ds_buffer_producers[ds_next_buffer_producer] = producer;
	ds_next_buffer_producer = (ds_next_buffer_producer + 1) % ds_buffer_producers.size();
	if (producer.shader == 0x6cbfe18ea2005263ull &&
	    producer.size == 65 * sizeof(vk::DispatchIndirectCommand)) {
		ds_light_queue_writer = producer;
	}
	return producer.serial;
}

bool DsTraceEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_DS_FRAME_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

void RememberDsBufferProducers(RenderContext& context, const PreparedBindings& bindings,
                               bool indirect, uint64_t submit) {
	if (!DsTraceEnabled()) return;
	const auto& program = *bindings.runtime->program;
	const auto frame = context.GetGpu().GetFrameNum();
	for (size_t i = 0; i < program.info.buffers.size(); ++i) {
		const auto& resource = program.info.buffers[i];
		if (!resource.written || resource.descriptor_index >= bindings.buffer_sources.size()) continue;
		const auto& range = bindings.buffer_sources[resource.descriptor_index];
		if (range.address == 0 || range.size == 0) continue;
		const auto native = DecodeNativeDescriptor<ShaderBufferResource>(
		    bindings.runtime->resources->buffers[i]);
		const uint64_t slack = resource.written_slack;
		uint64_t address = range.address;
		uint64_t size = range.size;
		if (!resource.written_dynamic && resource.written_begin < resource.written_end &&
		    !native.AddTid() && !native.SwizzleEnabled() && resource.written_begin >= slack &&
		    uint64_t {resource.written_end} + slack <= range.size) {
			address += resource.written_begin - slack;
			size = uint64_t {resource.written_end} - resource.written_begin + 2 * slack;
		}
		const auto serial = RememberDsBufferProducer(
		    {address, size, program.shader_hash, frame, indirect, false, submit});
		if (range.size == DemonsLightingMemoryPolicy::ProbeCoefficientBytes) {
			ds_probe_buffer_writes[ds_next_probe_write] =
			    {address, size, program.shader_hash, frame, indirect, false, submit, serial};
			ds_next_probe_write = (ds_next_probe_write + 1) % ds_probe_buffer_writes.size();
		}
	}
}

const DsBufferProducer* FindDsBufferProducer(int32_t frame, uint64_t address, uint64_t size,
                                            bool recent_only = true) {
	if (address == 0 || size == 0) return nullptr;
	for (size_t i = 0; i < ds_buffer_producers.size(); ++i) {
		const auto pos = (ds_next_buffer_producer + ds_buffer_producers.size() - 1 - i) %
		                 ds_buffer_producers.size();
		const auto& writer = ds_buffer_producers[pos];
		if (writer.size == 0 || writer.frame > frame ||
		    (recent_only && writer.frame < frame - 1)) continue;
		const bool overlap = address >= writer.address ? address - writer.address < writer.size
		                                              : writer.address - address < size;
		if (overlap) return &writer;
	}
	return nullptr;
}

void LogDsBufferProducer(int32_t frame, uint64_t shader, uint64_t args_addr, const char* role,
                         uint32_t index, uint64_t address, uint64_t size) {
	if (address == 0 || size == 0) return;
	const auto* candidate = FindDsBufferProducer(frame, address, size);
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_BUFFER_PRODUCER] frame={} shader=0x{:016x} args=0x{:x} role={} index={} "
	    "address=0x{:x} bytes={} candidate={} producer_shader=0x{:016x} "
	    "producer_frame={} producer_base=0x{:x} producer_bytes={} producer_indirect={} producer_submit={}\n",
	    frame, shader, args_addr, role, index, address, size,
	    candidate ? (candidate->fill ? "buffer_fill" : "bound_write") : "unobserved",
	    candidate ? candidate->shader : 0,
	    candidate ? candidate->frame : -1, candidate ? candidate->address : 0,
	    candidate ? candidate->size : 0, candidate && candidate->indirect ? 1 : 0,
	    candidate ? candidate->submit : 0));
}

} // namespace

static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::UpdateComputeMetaClears(const ShaderComputeInputInfo& input,
                                            const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	// Write-only compute metadata updates are not equivalent to a complete
	// HTile clear. The 48cc9dd D32 capture showed 1.44M populated pixels
	// lost on an implicit loadOp CLEAR while the guest depth_clear flag
	// was false. Execute the real shader; proven uniform fills are
	// subsequently recorded by TryConsumeComputeImageClear.
	static const bool ds_htile_safe = [] {
		const char* v = std::getenv("KYTY_DS_HTILE_FILL_SEMANTICS");
		return v && v[0] == '1' && v[1] == '\0';
	}();
	const auto invalidate_unproven = [&](uint64_t addr) {
		if (DemonsPcRenderParity::Enabled() || !HtileMetaWritePolicy::MustInvalidatePendingClear(ds_htile_safe, true, false) ||
		    !cache.InvalidateUnprovenHtileWrite(addr)) return false;
		static std::atomic<uint32_t> noted {0};
		const auto count = noted.fetch_add(1u, std::memory_order_relaxed);
		if (count < 24u || count % 4096u == 0u)
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_HTILE_WRITE] frame={} shader=0x{:016x} meta=0x{:x} "
			    "action=invalidate_unproven_clear dispatch_kept=1 count={}\n",
			    buffer.GetContext().GetGpu().GetFrameNum(), program.shader_hash, addr, count));
		return true;
	};
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// Reads and transforms of metadata do not establish a clear state.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			if (resource.written) (void)invalidate_unproven(descriptor.Base48());
			return false;
		}
	}

	if (program.info.has_bitwise_xor && ds_htile_safe) {
		for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
			if (!program.info.buffers[i].written) continue;
			const auto desc = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			(void)invalidate_unproven(desc.Base48());
		}
	}
	bool cleared=false;
	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (!invalidate_unproven(descriptor.Base48())) {
					if (DemonsPcRenderParity::Enabled()) cleared|=cache.ClearMetaRange(descriptor.Base48(),descriptor.GetSize(),0,false,"compute_meta",program.shader_hash);
					else (void)cache.ClearMeta(descriptor.Base48());
				}
			}
		}
	}
	return cleared;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	// Offset-or-swizzle bounds do not describe the element range proved by the shader.
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.OutOfBounds() == 3 || descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	const auto elements = descriptor.NumRecords();
	const bool covered = fill.bounded_elements != 0
	                         ? fill.bounded_elements == elements && invocations >= elements
	                         : invocations == elements;
	if (!covered || invocations > uint64_t {UINT32_MAX} + 1 || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		cache.ClearImage(command, binding.image_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		// PC KytyPlus 8744c436: proven uniform fills of uncached buffers need no
		// guest compute dispatch. Keep ownership and ordering via BufferCache.
		// UFC6's overlapping stencil/storage owners must keep the original shader:
		// it can have additional writes beyond the packed stencil clear.
		const bool overlap = cache.HasCachedImageOverlap(descriptor.Base48(), size);
		if (!ComputeBufferFillPolicy::UseUncachedBufferFill(descriptor.Base48(), size, overlap))
			return false;
		command.GetContext().GetBufferCache().FillBuffer(descriptor.Base48(), size, packed_clear,
		                                                 false);
		if (DemonsPcRenderParity::Enabled()) (void)cache.ClearMetaRange(descriptor.Base48(),size,packed_clear,true,"compute_uniform_fill",program.shader_hash);
		else (void)cache.ClearMeta(descriptor.Base48(), packed_clear);
		if (DsTraceEnabled()) {
			RememberDsBufferProducer({descriptor.Base48(), size, input.stage.program->shader_hash,
			                         command.GetContext().GetGpu().GetFrameNum(), false, true});
		}
		return true;
	}
	if (DemonsPcRenderParity::Enabled()) (void)cache.ClearMetaRange(descriptor.Base48(),size,packed_clear,true,"compute_uniform_fill",program.shader_hash);
		else (void)cache.ClearMeta(descriptor.Base48(), packed_clear);
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

void RenderExecutor::RememberDsAtlasWriter(ImageId id, const ImageInfo& info,
                                            const ImageViewInfo& view, uint64_t shader,
                                            const char* path) {
	if (DemonsRadianceTrace::Enabled() && id && !info.data.Empty()) {
		const auto& image = m_context.GetTextureCache().GetImage(id);
		const auto& owner = image.info.data;
		if (DemonsRadianceTrace::Data(info.data.address, info.data.size) ||
		    DemonsRadianceTrace::Data(owner.address, owner.size)) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_RADIANCE_IMAGE_WRITER] frame={} tick={} path={} shader=0x{:016x} "
			    "guest=0x{:x} bytes={} owner=0x{:x} version={} mip={} layer={} layers={}\n",
			    m_context.GetGpu().GetFrameNum(), m_context.GetCommandScheduler().CurrentTick(),
			    path, shader, info.data.address, info.data.size, owner.address,
			    image.ContentVersion(), view.base_level, view.base_layer, view.layer_count));
		}
	}
	static const bool trace = [] {
		const char* value = std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");
		return value && value[0] == '1' && value[1] == '\0';
	}();
	if (!trace || !id || info.data.Empty()) return;
	const auto& image = m_context.GetTextureCache().GetImage(id);
	const auto& data = image.info.data;
	const bool matches = m_ds_atlas_size && data.address < m_ds_atlas_address + m_ds_atlas_size &&
	                     m_ds_atlas_address < data.address + data.size;
	const bool shape = image.backing.format == vk::Format::eR16G16B16A16Sfloat &&
	                   image.backing.extent.width == 256 && image.backing.extent.height == 256 &&
	                   image.backing.layers == 360;
	if (!matches && !shape) return;
	const auto frame = m_context.GetGpu().GetFrameNum();
	if (m_ds_atlas_writer_count) {
		const auto& last = m_ds_atlas_writers[(m_ds_atlas_writer_count - 1) % m_ds_atlas_writers.size()];
		if (last.frame == frame && last.shader == shader && last.owner == data.address &&
		    last.layer == view.base_layer && last.layers == view.layer_count &&
		    last.mip == view.base_level && last.path == path) return;
	}
	auto& writer = m_ds_atlas_writers[m_ds_atlas_writer_count++ % m_ds_atlas_writers.size()];
	writer = {shader, info.data.address, info.data.size, data.address, image.ContentVersion(),
	          frame, view.base_layer, view.layer_count, view.base_level, path};
}

void RenderExecutor::BindDsCountArithmetic(PreparedBindings& bindings,bool capture) {
    namespace P=DemonsCountArithmeticProbe;
    const auto& p=*bindings.runtime->program;
    if(p.shader_hash!=P::Shader||!P::Supported(p.ds_light_math_variant))return;
    auto& storage=m_ds_count_arithmetic_buffers[capture];
    const bool init=!storage;
    constexpr auto bytes=P::WordCount*sizeof(uint32_t);
    if(init)storage=std::make_unique<Buffer>(m_context.GetGraphics(),m_context.GetCommandScheduler(),
        MemoryUsage::Download,0,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc,bytes);
    if(init||capture){
        std::array<uint32_t,P::WordCount> words{};
        words[0]=capture?1u:0u;
        std::memcpy(storage->Mapped().data(),words.data(),bytes);storage->Flush(0,bytes);
    }
    bindings.shared_memory=vk::DescriptorBufferInfo{storage->Handle(),0,bytes};
}
void RenderExecutor::BindDsCountExec(PreparedBindings& bindings,bool capture) {
    if(bindings.runtime->program->shader_hash==DemonsCountArithmeticProbe::Shader) {
        BindDsCountArithmetic(bindings,capture);return;
    }
    namespace D=DemonsCountExecProbe;
    const auto& p=*bindings.runtime->program;
    if(p.shader_hash!=D::Shader || !D::SupportedFingerprint(p.ds_light_math_variant))return;
    auto& storage=m_ds_count_exec_buffers[capture];
    const bool initialize=!storage;
    constexpr auto bytes=D::WordCount*sizeof(uint32_t);
    if(initialize)storage=std::make_unique<Buffer>(m_context.GetGraphics(),m_context.GetCommandScheduler(),
        MemoryUsage::Download,0,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc,bytes);
    if(initialize||capture){
        std::vector<uint32_t> data(D::WordCount);
        data[0]=capture?1u:0u;
        std::memcpy(storage->Mapped().data(),data.data(),bytes);
        storage->Flush(0,bytes);
    }
    bindings.shared_memory=vk::DescriptorBufferInfo{storage->Handle(),0,bytes};
}

void RenderExecutor::BindDsRelightTrace(PreparedBindings& bindings,std::array<uint32_t,3> header) {
    namespace P=DemonsRelightProbe;
    auto& storage=m_ds_relight_buffers[header[0]!=0];
    const bool init=!storage;
    const auto bytes=(header[0]?P::WordCount:P::Header)*sizeof(uint32_t);
    if(init)storage=std::make_unique<Buffer>(m_context.GetGraphics(),m_context.GetCommandScheduler(),
        MemoryUsage::Download,0,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc,bytes);
    if(init||header[0]){
        std::vector<uint32_t> words(header[0]?P::WordCount:P::Header);
        std::copy(header.begin(),header.end(),words.begin());
        std::memcpy(storage->Mapped().data(),words.data(),bytes);storage->Flush(0,bytes);
    }
    bindings.shared_memory=vk::DescriptorBufferInfo{storage->Handle(),0,bytes};
}

void RenderExecutor::BindDsLightMath(PreparedBindings& bindings,
                                    const DemonsQueueProbe::Capture* capture) {
	using namespace DemonsLightMathProbe;
	if (!bindings.runtime->program->ds_light_math_variant) return;
    if(bindings.runtime->program->shader_hash==DemonsRelightProbe::Shader){
        BindDsRelightTrace(bindings,{0,UINT32_MAX,UINT32_MAX});return;
    }
    if(bindings.runtime->program->shader_hash==DemonsCountArithmeticProbe::Shader){
        BindDsCountArithmetic(bindings,false);return;
    }
    if(bindings.runtime->program->shader_hash==DemonsCountExecProbe::Shader){
        BindDsCountExec(bindings,false);return;
    }
	if (bindings.images.size() > 7) {
		const auto& binding = bindings.images[7];
		if (binding.image_id && !binding.placeholder) {
			const auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
			if (image.backing.layers == 360 && image.backing.format == vk::Format::eR16G16B16A16Sfloat) {
				m_ds_atlas_address = image.info.data.address;
				m_ds_atlas_size = image.info.data.size;
			}
		}
	}
	auto& storage = m_ds_light_math_buffers[static_cast<bool>(capture)];
	const bool initialize = !storage;
	if (initialize) {
		storage = std::make_unique<Buffer>(m_context.GetGraphics(), m_context.GetCommandScheduler(),
		    MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eStorageBuffer,
		    WordCount * sizeof(uint32_t));
	}
	if (initialize || capture) {
		std::array<uint32_t, WordCount> data {};
		std::fill_n(data.begin(), Samples, UINT32_MAX);
		if (capture) {
			for (uint32_t i = 0; i < Samples; ++i) {
				const auto [x, y] = capture->pixels[i];
				if (x < 65535 && y < 65535) data[i] = x | (y << 16);
			}
		}
		std::memcpy(storage->Mapped().data(), data.data(), sizeof(data));
		storage->Flush(0, sizeof(data));
	}
	bindings.shared_memory = vk::DescriptorBufferInfo{storage->Handle(), 0,
	                                                   WordCount * sizeof(uint32_t)};
}

static void ReadDsAtlasTexels(RenderContext& context, const TextureBinding& binding,
                              uint32_t frame, uint64_t submit,
                              const std::array<uint32_t, DemonsLightMathProbe::WordCount>& data) {
	using namespace DemonsLightMathProbe;
	if (!binding.image_id || binding.placeholder) return;
	auto& image = context.GetTextureCache().GetImage(binding.image_id);
	const auto found = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
	if (found == image.views.end()) return;
	const auto view = found->info;
	const auto& host = image.backing;
	if (host.format != vk::Format::eR16G16B16A16Sfloat || host.samples != 1 ||
	    !(host.usage & vk::ImageUsageFlagBits::eTransferSrc) || view.base_level >= host.mip_levels ||
	    view.base_layer >= host.layers ||
	    (view.type != vk::ImageViewType::e2D && view.type != vk::ImageViewType::e2DArray)) return;
	const auto width = std::max(host.extent.width >> view.base_level, 1u);
	const auto height = std::max(host.extent.height >> view.base_level, 1u);
	if (width < 2 || height < 2) return;
	struct Sample { uint32_t pixel, site, x, y, layer; };
	std::array<Sample, Samples * 2> samples {};
	uint32_t count = 0;
	for (uint32_t i = 0; i < Samples; ++i) {
		for (uint32_t site = 0; site < 2; ++site) {
			const auto* term = data.data() + Samples + i * Stride + TermOffset + site * TermStride;
			if (!term[0]) continue;
			const auto u = std::bit_cast<float>(term[9]);
			const auto v = std::bit_cast<float>(term[10]);
			const auto z = std::bit_cast<float>(term[11]);
			if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(z) ||
			    u <= 0.f || u >= 1.f || v <= 0.f || v >= 1.f || z < 0.f ||
			    z >= static_cast<float>(std::min(view.layer_count, host.layers - view.base_layer)))
				continue;
			const auto layer = view.base_layer + static_cast<uint32_t>(std::floor(z + 0.5f));
			const auto x = static_cast<int32_t>(std::floor(u * static_cast<float>(width) - 0.5f));
			const auto y = static_cast<int32_t>(std::floor(v * static_cast<float>(height) - 0.5f));
			// Interior footprints avoid assumptions about addressing, border colors and seams.
			if (x < 0 || y < 0 || uint32_t(x + 1) >= width || uint32_t(y + 1) >= height ||
			    layer >= host.layers || layer - view.base_layer >= view.layer_count) continue;
			samples[count++] = {i, site, static_cast<uint32_t>(x), static_cast<uint32_t>(y), layer};
		}
	}
	if (!count) return;
	constexpr uint32_t bytes_per_footprint = 32;
	const auto bytes = count * bytes_per_footprint;
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(bytes, 16u, true);
	if (!mapped) return;
	download.Commit();
	std::array<vk::BufferImageCopy, Samples * 2> copies {};
	for (uint32_t i = 0; i < count; ++i) {
		const auto& sample = samples[i];
		auto& copy = copies[i];
		copy.bufferOffset = offset + i * bytes_per_footprint;
		copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		copy.imageSubresource.mipLevel = view.base_level;
		copy.imageSubresource.baseArrayLayer = sample.layer;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset = vk::Offset3D {static_cast<int32_t>(sample.x), static_cast<int32_t>(sample.y), 0};
		copy.imageExtent = vk::Extent3D {2, 2, 1};
	}
	image.Download(std::span{copies}.first(count), download.Handle(), offset, bytes);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, bytes);
	for (uint32_t i = 0; i < count; ++i) {
		const auto& sample = samples[i];
		std::array<uint16_t, 16> texels {};
		std::memcpy(texels.data(), mapped + i * bytes_per_footprint, bytes_per_footprint);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_LIGHT_ATLAS_TEXELS] frame={} submit={} sample={} site={} layer={} xy={},{} "
		    "rgb16={:04x}/{:04x}/{:04x},{:04x}/{:04x}/{:04x},"
		    "{:04x}/{:04x}/{:04x},{:04x}/{:04x}/{:04x}\n",
		    frame, submit, sample.pixel, sample.site, sample.layer, sample.x, sample.y,
		    texels[0], texels[1], texels[2], texels[4], texels[5], texels[6],
		    texels[8], texels[9], texels[10], texels[12], texels[13], texels[14]));
	}
}

void RenderExecutor::ReadDsLightMath(const DemonsQueueProbe::Capture& capture, uint64_t submit,
                                    const PreparedBindings& bindings) {
	using namespace DemonsLightMathProbe;
	auto& storage = m_ds_light_math_buffers[1];
	m_context.GetCommandScheduler().Finish();
	std::array<uint32_t, WordCount> data {};
	storage->Invalidate(0, sizeof(data));
	std::memcpy(data.data(), storage->Mapped().data(), sizeof(data));
	// These descriptor slots are fixed in the two verified queue 33 binaries.
	const auto variant = bindings.runtime->program->ds_light_math_variant;
	for (const uint32_t index : {6u, 7u}) {
		if (index >= bindings.images.size()) continue;
		const auto& binding = bindings.images[index];
		if (!binding.image_id || binding.placeholder) continue;
		const auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
		const auto found = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
		if (found == image.views.end()) continue;
		const auto& view = found->info;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_LIGHT_TERM_VIEW] frame={} submit={} variant={:016x} slot={} guest=0x{:x} "
		    "owner=0x{:x} version={} view={} base_layer={} layer_count={} base_mip={}\n",
		    capture.frame, submit, variant, index, binding.desc.info.data.address,
		    image.info.data.address, image.ContentVersion(), vk::to_string(view.type),
		    view.base_layer, view.layer_count, view.base_level));
	}
	const auto float_at = [](const uint32_t* row, size_t i) { return std::bit_cast<float>(row[i]); };
	for (uint32_t i = 0; i < Samples; ++i) {
		const auto* row = data.data() + Samples + i * Stride;
		const auto [x, y] = capture.pixels[i];
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_LIGHT_MATH] frame={} submit={} sample={} xy={},{} wrote_hdr={} "
		    "albedo=0x{:08x} depth={:.8g} stencil=0x{:02x} accumulated={:.8g}/{:.8g}/{:.8g} "
		    "scale={:.8g} rgb_factors={:.8g}/{:.8g}/{:.8g} incoming={:.8g}/{:.8g}/{:.8g} "
		    "stored={:.8g}/{:.8g}/{:.8g} auxiliary={:.8g}\n",
		    capture.frame, submit, i, x, y, row[0] != 0, row[1], float_at(row, 2), row[3],
		    float_at(row, 4), float_at(row, 5), float_at(row, 6), float_at(row, 7),
		    float_at(row, 8), float_at(row, 9), float_at(row, 10),
		    float_at(row, 11), float_at(row, 12), float_at(row, 13),
		    float_at(row, 14), float_at(row, 15), float_at(row, 16), float_at(row, 17)));
		for (uint32_t site = 0; site < 2; ++site) {
			const auto* term = row + TermOffset + site * TermStride;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_LIGHT_TERM] frame={} submit={} sample={} site={} count={} "
			    "last_uv_layer={:.8g}/{:.8g}/{:.8g} last_light_rgb={:.8g}/{:.8g}/{:.8g} "
			    "last_weight={:.8g} last_shadow_result={:.8g} last_shadow_ref={:.8g} "
			    "last_visibility={:.8g} last_weighted={:.8g} "
			    "sum_before_shadow={:.8g}/{:.8g}/{:.8g} sum_after_shadow={:.8g}/{:.8g}/{:.8g}\n",
			    capture.frame, submit, i, site, term[0], float_at(term, 9), float_at(term, 10),
			    float_at(term, 11), float_at(term, 3), float_at(term, 4), float_at(term, 5),
			    float_at(term, 6), float_at(term, 1), float_at(term, 2),
			    float_at(term, 7), float_at(term, 8), float_at(term, 12), float_at(term, 13),
			    float_at(term, 14), float_at(term, 15), float_at(term, 16), float_at(term, 17)));
		}
	}
	if (bindings.images.size() > 7) {
		ReadDsAtlasTexels(m_context, bindings.images[7], capture.frame, submit, data);
	}
	const auto end = m_ds_atlas_writer_count;
	const auto count = std::min(end, static_cast<uint32_t>(m_ds_atlas_writers.size()));
	for (uint32_t i = end - count; i < end; ++i) {
		const auto& writer = m_ds_atlas_writers[i % m_ds_atlas_writers.size()];
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_LIGHT_ATLAS_WRITER] read_frame={} read_submit={} writer_frame={} shader=0x{:016x} "
		    "path={} guest=0x{:x} bytes=0x{:x} owner=0x{:x} version={} layer={} layers={} mip={}\n",
		    capture.frame, submit, writer.frame, writer.shader, writer.path, writer.address,
		    writer.size, writer.owner, writer.version, writer.layer, writer.layers, writer.mip));
	}
}

static void BindSharedMemory(RenderContext& context, ShaderComputeInputInfo& input,
                             PreparedBindings& bindings, uint64_t indirect_args = 0) {
	if (input.stage.program->ds_light_math_variant ||
	    input.stage.program->bindings.descriptor_counts[
	        static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] == 0) {
		return;
	}
	auto& cache = context.GetBufferCache();
	// On Demon's Souls, the lighting-list dispatch can generate its own indirect
	// group counts with a GPU shader. Guest RAM may still contain an older zero
	// even when the Vulkan argument buffer is authoritative. Reading it to size
	// the emulated LDS would allocate just four bytes and make every workgroup
	// access out of bounds. The shader indexes its LDS by gl_NumWorkGroups:
	// reserve the available GPU storage range instead of trusting stale host
	// counts. Do not add a GPU->CPU readback or a fence to this path.
	static const bool ds_gpu_lds_groups = [] {
		const char* value = std::getenv("KYTY_DS_GPU_LDS_GROUPS");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	// Even after publishing GPU arguments into BufferCache, dirty tracking can
	// become clean while CPU memory still holds the *old* group counts. The
	// previous dirty-only test then read zero from RAM and bound a four-byte
	// LDS descriptor for a valid vkCmdDispatchIndirect. Always reserve the
	// existing GPU utility-buffer range for this title's indirect workgroups.
	// Unlike forcing a CPU readback, this cannot stale the Vulkan arguments or
	// add a GPU/CPU synchronization point.
	const bool gpu_owned_args = ReserveIndirectLdsFromGpu(indirect_args, ds_gpu_lds_groups);
	if (indirect_args != 0 && !gpu_owned_args) {
		const Common::PerfStats::ScopedWaitSite site(Common::PerfStats::Wait::SemIndirectArgs);
		cache.ReadMemory(indirect_args, sizeof(vk::DispatchIndirectCommand));
		std::memcpy(input.workgroup_counts, reinterpret_cast<const void*>(indirect_args),
		            sizeof(input.workgroup_counts));
	}
	// LDS has no contents to preserve between dispatches. The existing shader hazard
	// barriers also order other users of this GPU-only utility buffer.
	auto& storage = cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
	const auto limit = std::min<uint64_t>(storage.Size(),
	    context.GetGraphics().GetPhysicalDeviceProperties().limits.maxStorageBufferRange);
	uint64_t size = gpu_owned_args ? limit : sizeof(uint32_t);
	if (!gpu_owned_args &&
	    std::ranges::find(input.workgroup_counts, 0u) == std::end(input.workgroup_counts)) {
		size = uint64_t {input.lds_size_dwords} * sizeof(uint32_t);
		EXIT_IF(size == 0 || size > limit);
		for (const auto count: input.workgroup_counts) {
			EXIT_IF(size > limit / count);
			size *= count;
		}
	}
	bindings.shared_memory = vk::DescriptorBufferInfo {storage.Handle(), 0, size};
}

static DispatchGroups ComputeGroupLimits(const GraphicContext& graphics) {
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits.maxComputeWorkGroupCount;
	return {limits[0], limits[1], limits[2]};
}

static bool LdsInStorage(const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	return program.bindings.descriptor_counts[static_cast<size_t>(
	           ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] != 0;
}

// Records one guest dispatch. Past maxComputeWorkGroupCount it is recorded as pieces within the
// limits (vkCmdDispatchBase cannot cross them: VUID-vkCmdDispatchBase-groupCountX-00424), each
// with its first group in the program's group-base shader data (Bindings::group_base_dword),
// which the translated shader adds to gl_WorkGroupID / gl_GlobalInvocationID. The
// LDS-in-storage-buffer fallback indexes its per-group slot with the piece's gl_NumWorkGroups:
// unique within a piece, not across pieces, so its pieces are serialized.
// groups counts host workgroups; a packed program (KYTY_CS_PACK, IR::ShaderInfo::group_pack) runs
// group_pack guest workgroups along X in each, so the pieces' bases are scaled back to guest groups.
static void RecordGuestDispatch(vk::CommandBuffer command, const DispatchGroups& limits,
                                const DispatchGroups&                           groups,
                                const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                const PipelineCache::Pipeline&                  pipeline) {
	const uint32_t pack = std::max(program.info.group_pack, 1u);
	if (DispatchFits(groups, limits)) {
		command.dispatch(groups[0], groups[1], groups[2]);
		return;
	}
	using ShaderRecompiler::IR::PushData;
	const auto& bindings       = program.bindings;
	const bool  reads_ids      = bindings.group_base_dword != PushData::NoStart;
	const bool  lds_in_storage = LdsInStorage(program);
	if (reads_ids && !bindings.UsesPushData()) {
		// Shader data in a uniform buffer: no per-piece base. Recorded unsplit, as before.
		static std::atomic<uint32_t> unsplit_count {0};
		if (unsplit_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Compute dispatch split: cs=0x%016" PRIx64 " groups=%ux%ux%u exceeds the limits "
			     "but its shader data is not push data; dispatched unsplit\n",
			     program.shader_hash, groups[0], groups[1], groups[2]);
		}
		command.dispatch(groups[0], groups[1], groups[2]);
		return;
	}
	const uint32_t base_offset =
	    reads_ids ? (bindings.push_data_start_dword + bindings.group_base_dword) * 4u : 0u;
	uint32_t   recorded = 0;
	const auto pieces   = ForEachDispatchPiece(groups, limits, [&](const DispatchPiece& piece) {
        if (lds_in_storage && recorded != 0) {
            vk::MemoryBarrier barrier {};
            barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                    vk::PipelineStageFlagBits::eComputeShader, {}, 1, &barrier, 0,
                                    nullptr, 0, nullptr);
        }
        if (reads_ids) {
            DispatchGroups base = piece.base;
            base[0] *= pack;
            command.pushConstants(pipeline.pipeline_layout, pipeline.push_constant_stages,
                                  base_offset, sizeof(base), base.data());
        }
        command.dispatch(piece.count[0], piece.count[1], piece.count[2]);
        recorded++;
    });
	if (reads_ids) {
		// Later dispatches of this pipeline without a re-commit see base 0 again.
		const DispatchGroups zero {};
		command.pushConstants(pipeline.pipeline_layout, pipeline.push_constant_stages, base_offset,
		                      sizeof(zero), zero.data());
	}
	static std::atomic<uint64_t> split_count {0};
	const auto                   count = split_count.fetch_add(1, std::memory_order_relaxed) + 1;
	if (count <= 16 || count % 4096 == 0) {
		LOGF("Compute dispatch split #%" PRIu64 ": cs=0x%016" PRIx64 " groups=%ux%ux%u > limits "
		     "%ux%ux%u -> %u pieces%s%s\n",
		     count, program.shader_hash, groups[0], groups[1], groups[2], limits[0], limits[1],
		     limits[2], pieces, reads_ids ? " (group base in push data)" : " (ids unused)",
		     lds_in_storage ? " (serialized: LDS in storage buffer)" : "");
	}
}

// Two-stage GPU thread sizing (pipelineProbe.h): BDA users and the materialized resource bytes.
static void NoteComputeItem(const ShaderComputeInputInfo& input_info) {
	if (!Common::PerfStats::Enabled() || input_info.stage.program == nullptr) return;
	static const bool logged = [] {
		LOGF("[pipe-probe] register snapshot candidates: HW::Context=%zu HW::UserConfig=%zu "
		     "HW::Shader=%zu bytes\n",
		     sizeof(HW::Context), sizeof(HW::UserConfig), sizeof(HW::Shader));
		return true;
	}();
	(void)logged;
	if (input_info.stage.program->has_address_writes) PipelineProbe::MarkDmaWriter();
	PipelineProbe::AddResourceBytes(PipelineProbe::ResourceBytes(input_info.stage.resources));
}

// PPSA01341: match a sampled pixel from a compute storage image with a
// downstream graphics render target. An accepted dispatch count alone cannot
// distinguish black lighting data from broken final composition.
static void ProbeDsComputeWrites(RenderContext& context,
                                 const ShaderComputeInputInfo& input,
                                 const PreparedBindings& bindings) {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DS_COLOR_PROBE");
		if (value != nullptr) return value[0] == '1' && value[1] == '\0';
		value = std::getenv("KYTY_DS_FRAME_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if ((!enabled && !DsTraceEnabled()) || !input.stage.program) return;
	const auto& images = input.stage.program->info.images;
	bool representative_probed = false;
	for (uint32_t i = 0; i < images.size() && i < bindings.images.size(); ++i) {
		if (!images[i].written ||
		    images[i].resource_class != ShaderRecompiler::IR::ImageResourceClass::Storage ||
		    !bindings.images[i].image_id || bindings.images[i].placeholder)
			continue;
		const uint64_t hash = input.stage.program->shader_hash;
		const ImageId target_id = bindings.images[i].image_id;
		if (enabled && !representative_probed) {
			ProbeDsComputeImage(context, target_id, hash);
			representative_probed = true;
		}
		// The 07:07 diagnostic identified six repeatedly black 2560x1440
		// RGBA16F writers at guest address 0x281300000. Compare their
		// *actual sampled inputs* against their zero outputs, once per
		// distinct shader. This determines whether lighting's inputs are
		// already black or its dispatch fails to produce color.
		constexpr std::array<uint64_t, 6> target_hashes {{
		    0x23056085c1120461ull, 0xe75589aa0bb3ae52ull,
		    0x3dfe402653925fbfull, 0x93b0be7f1a0293d1ull,
		    0xfabe0bcb275a3005ull, 0xc2e01fbe98f5ebabull
		}};
		// Keep a bounded continuous trace for any session length, without
		// guessed init/scene/gameplay frame boundaries.
		static std::array<DemonsRollingProbe::FrameGate, 6> sampled_writers {};
		const auto match = std::find(target_hashes.begin(), target_hashes.end(), hash);
		const auto ordinal = static_cast<size_t>(match - target_hashes.begin());
		const int32_t frame = context.GetGpu().GetFrameNum();
		static const bool targeted = [] {
			const char* v = std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");
			return v != nullptr && v[0] == '1' && v[1] == '\0';
		}();
		// The targeted capture below samples pixels from the actual light-tile queue.
		const bool probe_this_capture = enabled && !targeted;
		if (match != target_hashes.end() &&
		    context.GetTextureCache().GetImage(target_id).info.data.address == 0x281300000ull &&
		    sampled_writers[ordinal].Due(frame, 240)) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_TARGET_WRITER] shader=0x{:016x} output=0x{:x} "
			    "frame={} capture=rolling image_resources={} output_binding={} "
			    "indirect=0x{:x} group_pack={}\n",
			    hash, 0x281300000ull, frame,
			    images.size(), i, input.indirect_args_address,
			    input.stage.program->info.group_pack));
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_LIGHTING_FEATURES] shader=0x{:016x} bvh={} uses_dma={} "
			    "uses_lds={} shared_int64_atomics={} buffer_int64_atomics={} "
			    "fp64={} output=0x{:x}\n", hash,
			    input.stage.program->info.bvh ? 1 : 0,
			    input.stage.program->info.uses_dma ? 1 : 0,
			    input.stage.program->info.uses_lds ? 1 : 0,
			    input.stage.program->info.shared_int64_atomics ? 1 : 0,
			    input.stage.program->info.buffer_int64_atomics ? 1 : 0,
			    input.stage.program->info.float64 ? 1 : 0,
			    0x281300000ull));
			if (probe_this_capture) {
				const auto& target = context.GetTextureCache().GetImage(target_id);
				const auto view = std::ranges::find(target.views, bindings.images[i].image_view,
				                                    &CachedImageView::view);
				if (view != target.views.end())
					ProbeDsComputeImage(context, target_id, hash, true, &view->info);
			}
			// The previous diagnostic stopped after four inputs. For all six
			// affected shaders those were mostly the same black 4x4 image.
			// Log every *read* image descriptor, even an unbound one, but spend
			// synchronous GPU probes only on distinct, non-trivial inputs.
			// Never substitute a texture or alter descriptor bindings here.
			uint32_t inspected = 0, probed = 0, missing = 0, tiny = 0;
			std::array<std::tuple<ImageId, vk::ImageAspectFlags, uint32_t, uint32_t>, 32> seen_views {};
			uint32_t seen_count = 0;
			for (uint32_t other = 0; other < images.size() &&
			     other < bindings.images.size(); ++other) {
				if (other == i || !images[other].read) continue;
				++inspected;
				const auto& source = bindings.images[other];
				uint64_t raw_address = 0;
				uint32_t raw_width = 0, raw_height = 0;
				if (other < input.stage.resources->images.size()) {
					const auto native = DecodeNativeDescriptor<ShaderTextureResource>(
					    input.stage.resources->images[other]);
					raw_address = native.Base40();
					raw_width = static_cast<uint32_t>(native.Width5()) + 1u;
					raw_height = static_cast<uint32_t>(native.Height5()) + 1u;
				}
				if (!source.image_id || source.placeholder) {
					++missing;
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_TARGET_INPUT] shader=0x{:016x} index={} class={} "
					    "raw=0x{:x} raw_extent={}x{} unbound_or_placeholder=1\n",
					    hash, other, static_cast<uint32_t>(images[other].resource_class),
					    raw_address, raw_width, raw_height));
					continue;
				}
				const auto& texture = context.GetTextureCache().GetImage(source.image_id);
				const uint64_t addr = texture.info.data.address;
				const auto view = std::ranges::find(texture.views, source.image_view,
				                                    &CachedImageView::view);
				const bool has_view = view != texture.views.end();
				const auto& view_info = has_view ? view->info : source.desc.view_info;
				const auto aspect = has_view ? view_info.aspect : vk::ImageAspectFlags {};
				const auto key = std::tuple {source.image_id, aspect,
				                             view_info.base_layer, view_info.base_level};
				const auto end = seen_views.begin() + seen_count;
				const bool repeated = std::find(seen_views.begin(), end, key) != end;
				if (!repeated && seen_count < seen_views.size())
					seen_views[seen_count++] = key;
				const bool is_tiny =
				    texture.backing.extent.width <= 4u &&
				    texture.backing.extent.height <= 4u;
				if (is_tiny) ++tiny;
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_TARGET_INPUT] shader=0x{:016x} index={} class={} "
				    "raw=0x{:x} raw_extent={}x{} guest=0x{:x} format={} "
				    "extent={}x{} owner_layers={} view_layer={} view_mip={} view_aspect=0x{:x} "
				    "repeated={} tiny={} feedback={} gpu_modified={} cpu_dirty={}\n",
				    hash, other, static_cast<uint32_t>(images[other].resource_class),
				    raw_address, raw_width, raw_height, addr,
				    static_cast<uint32_t>(texture.backing.format),
				    texture.backing.extent.width, texture.backing.extent.height,
				    texture.backing.layers, view_info.base_layer, view_info.base_level,
				    static_cast<uint32_t>(aspect),
				    repeated ? 1 : 0, is_tiny ? 1 : 0,
				    source.image_id == target_id ? 1 : 0,
				    texture.IsGpuModified() ? 1 : 0, texture.IsCpuDirty() ? 1 : 0));
				if (probe_this_capture && has_view && !repeated && !is_tiny &&
				    texture.backing.extent.width >= 960u &&
				    texture.backing.extent.height >= 540u &&
			    source.image_id != target_id && probed < 6u) {
					ProbeDsComputeImage(context, source.image_id, hash, true, &view_info);
					++probed;
				}
			}
			// Compute illumination may source scalar constants and buffers even
			// when its sampled images are valid. Record bounded guest buffer
			// descriptors without dereferencing guest memory or changing binds.
			if (input.stage.resources != nullptr) {
				const auto& snapshot = *input.stage.resources;
				for (uint32_t bi = 0; bi < input.stage.program->info.buffers.size() &&
				     bi < snapshot.buffers.size(); ++bi) {
					const auto& resource = input.stage.program->info.buffers[bi];
					const auto native = DecodeNativeDescriptor<ShaderBufferResource>(
					    snapshot.buffers[bi]);
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_TARGET_BUFFER] shader=0x{:016x} index={} source={} "
					    "guest=0x{:x} stride={} records={} format={} written={}\n",
					    hash, bi, resource.source, native.Base48(), native.Stride(),
					    native.NumRecords(), native.RawFormat(), resource.written ? 1 : 0));
				}
			}
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_TARGET_INPUT_SUMMARY] shader=0x{:016x} read_images={} "
			    "probed={} missing={} tiny={} buffers={} dma={} address_writes={}\n",
			    hash, inspected, probed, missing, tiny, input.stage.program->info.buffers.size(),
			    input.stage.program->info.uses_dma ? 1 : 0,
			    input.stage.program->has_address_writes ? 1 : 0));
			return;
		}
	}
}

// Read the native Vulkan buffer, not stale guest backing, for the live Demon's Souls
// lighting list and the two shaders that produce its inputs/indirect counts.
// Uses the existing DS trace setting and is sampled across the entire run, not at
// a hard-coded frame. One copy per matching bound range, no changes to guest memory.
static void ProbeDsLightingBuffers(RenderContext& context, const PreparedBindings& bindings) {
	if (!DsTraceEnabled() || !bindings.runtime || !*bindings.runtime) return;
	const auto& program = *bindings.runtime->program;
	const uint64_t shader = program.shader_hash;
	static constexpr std::array<uint64_t, 8> watched {
	    0x3dfe402653925fbfull, 0x6cbfe18ea2005263ull,
	    0x1ce1a858f62b5a11ull, 0xc2e01fbe98f5ebabull,
	    0x23056085c1120461ull, 0xfabe0bcb275a3005ull,
	    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
	};
	const auto which = std::find(watched.begin(), watched.end(), shader);
	if (which == watched.end()) return;
	static std::array<DemonsRollingProbe::FrameGate, watched.size()> gates {};
	const auto frame = context.GetGpu().GetFrameNum();
	const auto watch_index = static_cast<size_t>(which - watched.begin());
	if (!gates[watch_index].Due(frame, 240)) return;
	static std::array<uint32_t, watched.size()> count {};
	if (count[watch_index]++ >= 6) return;
	// This is deliberately unconditional: LOGF was suppressed for this build's
	// file-logging configuration, so the earlier capture never appeared in its ZIP.
	Log::WriteToConsoleAndLog(fmt::sprintf(
	    "[DS_GPU_CAPTURE_START] frame=%d shader=0x%016" PRIx64
	    " buffers=%zu images=%zu samplers=%zu\n",
	    frame, shader, program.info.buffers.size(),
	    program.info.images.size(), program.info.samplers.size()));
	// Check every compared shadow map/sampler pair, regardless of whether its
	// indirect lighting dispatch has X=0 for this frame.
	const auto& snapshots = *bindings.runtime->resources;
	for (uint32_t index = 0; index < program.info.samplers.size() &&
	     index < snapshots.samplers.size(); ++index) {
		const auto& metadata = program.info.samplers[index];
		const auto desc = DecodeNativeDescriptor<ShaderSamplerResource>(snapshots.samplers[index]);
		Log::WriteToConsoleAndLog(fmt::sprintf(
		    "[DS_GPU_SAMPLER] frame=%d shader=0x%016" PRIx64
		    " index=%u dref=%u compare=%u unnormalized=%u raw=%08x,%08x,%08x,%08x\n",
		    frame, shader, index, metadata.depth_compare ? 1u : 0u,
		    static_cast<unsigned>(desc.DepthCompareFunc()),
		    desc.ForceUnormCoords() ? 1u : 0u,
		    desc.fields[0], desc.fields[1], desc.fields[2], desc.fields[3]));
	}
	auto& scheduler = context.GetCommandScheduler();
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	uint32_t first_tile = 0, end_tile = 0;
	DsProbePixels pixels {};
	bool have_tiles = false;
	uint32_t captured = 0;
	for (uint32_t i = 0; i < program.info.buffers.size() && captured < 4; ++i) {
		const auto& resource = program.info.buffers[i];
		const uint32_t slot = resource.descriptor_index;
		if (slot == UINT32_MAX || slot >= bindings.buffers.size() ||
		    slot >= bindings.buffer_sources.size()) continue;
		const auto& source = bindings.buffer_sources[slot];
		const auto& native = bindings.buffers[slot];
		if (source.address == 0 || source.size < 4 || !native.buffer ||
		    native.range == VK_WHOLE_SIZE || native.range < 4) continue;
		const bool reader = shader == watched[0] && !resource.written &&
		                    (resource.source <= 1u || resource.source == 8u || resource.source == 11u);
		const bool other_reader = watch_index >= 3u && !resource.written &&
		                          captured < 2u;
		const bool args_writer = shader == watched[1] && resource.written &&
		                         source.size <= 65536u;
		const bool list_writer = shader == watched[2] && resource.written &&
		                         source.size >= 4096u;
		if (!reader && !other_reader && !args_writer && !list_writer) continue;
		const auto offset_word = program.bindings.memory_offset_dword + slot / 4u;
		if (offset_word >= bindings.shader_data.size()) continue;
		const auto adjustment = (bindings.shader_data[offset_word] >> ((slot % 4u) * 8u)) & 255u;
		if (adjustment >= native.range) continue;
		const uint64_t size = (std::min<uint64_t>(
		    {source.size, native.range - adjustment, 512u * 1024u}) / 4u) * 4u;
		if (size < 4) continue;
		auto [mapped, dest] = download.Map(size, 16u, true);
		if (mapped == nullptr) {
			Log::WriteToConsoleAndLog(fmt::sprintf("[DS_GPU_BUFFER_CAPTURE] frame=%d shader=0x%016" PRIx64
			     " source=%u error=staging_unavailable bytes=%" PRIu64 "\n",
			     frame, shader, resource.source, size));
			continue;
		}
		download.Commit();
		scheduler.EndRendering(PassEnd::Buffer);
		const auto command = scheduler.Current().Handle();
		vk::MemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eShaderWrite |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                        vk::PipelineStageFlagBits::eTransfer, {}, 1, &before,
		                        0, nullptr, 0, nullptr);
		const vk::BufferCopy copy {native.offset + adjustment, dest, size};
		command.copyBuffer(native.buffer, download.Handle(), 1, &copy);
		vk::MemoryBarrier after {};
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eHostRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                        vk::PipelineStageFlagBits::eHost, {}, 1, &after, 0, nullptr, 0, nullptr);
		scheduler.Finish();
		download.Invalidate(dest, size);
		const uint64_t words = size / 4u;
		uint64_t digest = 14695981039346656037ull;
		for (uint64_t b = 0; b < size; ++b) {
			digest = (digest ^ mapped[b]) * 1099511628211ull;
		}
		const auto at = [&](uint64_t index) {
			uint32_t value {};
			std::memcpy(&value, mapped + 4u * std::min(index, words - 1u), 4);
			return value;
		};
		if (shader == watched[0] && resource.source == 0 && words > 9) {
			first_tile = at(8);
			end_tile = at(9);
		}
		if (shader == watched[0] && resource.source == 1 && end_tile > first_tile &&
		    end_tile <= words && snapshots.flattened_srt.size() > 114) {
			const auto pitch = snapshots.flattened_srt[112];
			const auto width = snapshots.flattened_srt[113];
			const auto height = snapshots.flattened_srt[114];
			// The guest shader divides a queued tile ID by the pitch and addresses
			// an 8x8 region. Pick tile centers from across the recorded queue.
			have_tiles = pitch != 0 && uint64_t{pitch} == (uint64_t{width} + 7) / 8 &&
			             width != 0 && height != 0;
			for (size_t i = 0; have_tiles && i < pixels.size(); ++i) {
				const auto tile = at(first_tile + (end_tile - first_tile - 1u) * i / (pixels.size() - 1u));
				const uint64_t x = uint64_t{tile % pitch} * 8;
				const uint64_t y = uint64_t{tile / pitch} * 8;
				have_tiles = x < width && y < height;
				pixels[i] = {static_cast<uint32_t>(std::min<uint64_t>(x + 4, width - 1)),
				             static_cast<uint32_t>(std::min<uint64_t>(y + 4, height - 1))};
			}
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_LIGHT_TILE_PROBE] frame={} shader=0x{:016x} first={} end={} "
			    "pitch={} width={} height={} valid={} xy={},{}/{},{}/{},{}/{},{}/{},{}\n",
			    frame, shader, first_tile, end_tile, pitch, width, height, have_tiles,
			    pixels[0].first, pixels[0].second, pixels[1].first, pixels[1].second,
			    pixels[2].first, pixels[2].second, pixels[3].first, pixels[3].second,
			    pixels[4].first, pixels[4].second));
		}
		uint64_t active = 0;
		for (uint64_t i = 0; i < words; ++i) active += at(i) != 0u;
		std::array<uint32_t, 4> backing {};
		const bool backing_ok = LibKernel::Memory::TryReadBacking(
		    source.address, backing.data(), sizeof(backing));
		Log::WriteToConsoleAndLog(fmt::sprintf("[DS_GPU_BUFFER_CAPTURE] frame=%d shader=0x%016" PRIx64
		     " role=%s source=%u guest=0x%016" PRIx64
		     " slot=%u declared=%" PRIu64 " copied=%" PRIu64
		     " partial=%u active_words=%" PRIu64 "/%" PRIu64
		     " digest=%016" PRIx64
		     " first=%08x,%08x,%08x,%08x quarter=%08x half=%08x "
		     "three_quarters=%08x last=%08x "
		     "backing_ok=%u backing_first=%08x,%08x,%08x,%08x\n",
		     frame, shader, resource.written ? "producer" : "consumer",
		     resource.source, source.address, slot, source.size, size,
		     size < source.size ? 1u : 0u, active, words, digest,
		     at(0), at(1), at(2), at(3), at(words/4u), at(words/2u),
		     at(words*3u/4u), at(words-1u), backing_ok ? 1u : 0u,
		     backing[0], backing[1], backing[2], backing[3]));
		// Persist the exact bytes of this bound GPU buffer, not only a five-word
		// histogram. This data distinguishes empty lists from wrong addresses
		// and enables offline decoding of the guest light structures.
		const auto root = Config::GetShaderDumpFolder();
		if (!root.empty() && root.filename() == "ds-lighting-dumps") {
			const auto folder = root / "buffers";
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);
			if (!ec) {
				const auto name = fmt::sprintf("cs_%016" PRIx64 "_f%u_s%u_a%016" PRIx64 ".bin",
				                               shader, static_cast<uint32_t>(frame),
				                               resource.source, source.address);
				std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
				if (file) {
					file.write(reinterpret_cast<const char*>(mapped),
					           static_cast<std::streamsize>(size));
				}
				Log::WriteToConsoleAndLog(fmt::sprintf(
				    "[DS_GPU_BUFFER_FILE] frame=%d shader=0x%016" PRIx64
				    " file=%s bytes=%" PRIu64 " written=%u\n",
				    frame, shader, name.c_str(), size, file.good() ? 1u : 0u));
			}
		}
		++captured;
	}
	if (have_tiles) {
		for (const auto& binding: bindings.images) {
			if (!binding.image_id || binding.placeholder) continue;
			const auto& image = context.GetTextureCache().GetImage(binding.image_id);
			const auto view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
			if (view == image.views.end()) continue;
			const bool hdr = binding.desc.type == TextureCache::BindingType::Storage &&
			                 image.info.data.address == 0x281300000ull;
			const bool stencil = view->info.aspect == vk::ImageAspectFlagBits::eStencil;
			if (!hdr && !stencil) continue;
			const auto& mapping = view->info.mapping;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_LIGHT_TILE_VIEW] frame={} role={} swizzle={}/{}/{}/{}\n", frame,
			    hdr ? "hdr" : "stencil", static_cast<uint32_t>(mapping.r),
			    static_cast<uint32_t>(mapping.g), static_cast<uint32_t>(mapping.b),
			    static_cast<uint32_t>(mapping.a)));
			ProbeDsComputeImage(context, binding.image_id, shader, true, &view->info, &pixels);
		}
	}
	if (captured == 0) {
		Log::WriteToConsoleAndLog(fmt::sprintf("[DS_GPU_BUFFER_CAPTURE] frame=%d shader=0x%016" PRIx64
		     " no_matching_binding=1 resources=%zu\n", frame, shader,
		     program.info.buffers.size()));
	}
}

// KYTY_CS_PACK runs two guest workgroups of at most 32 threads in one 64-lane host workgroup (the
// recompiler decides per program: ShaderRecompiler.cpp ComputeGroupPack). Default on with Mesa
// Turnip (Astro Bot: 74.8 -> 66.7 GPU ms/frame, research/clock_policy.md), off elsewhere;
// KYTY_CS_PACK=0/1 overrides, and with KYTY_LIVE_KNOBS=1 the live key cs_pack=0/1 overrides both
// (in-run A/B; each setting is its own pipeline).
static bool ComputeGroupPackEnabled(bool turnip) {
	static const bool default_on = [turnip] {
		const char* value = std::getenv("KYTY_CS_PACK");
		const bool  on    = value != nullptr && value[0] != '\0' ? std::strcmp(value, "0") != 0 : turnip;
		LOGF("KYTY_CS_PACK: packing small compute workgroups in pairs %s\n", on ? "on" : "off");
		return on;
	}();
	return LiveKnobs::Int("cs_pack", default_on ? 1 : 0) != 0;
}

// Title-specific native SPIR-V BVH/SRT producer ordering (no ray stub).
static bool DemonsBvhSrtCoherence() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_DS_BVH_SRT_COHERENCE");
		return v != nullptr && v[0] == '1' && v[1] == '\0';
	}();
	return enabled;
}

// KYTY_LIVE_KNOBS bvh_opt=<0..31>: BVH emulation bits for programs compiled from now on
// (ShaderComputeInputInfo::bvh_live); absent keeps KYTY_BVH_OPT / KYTY_BVH_VEC.
static uint8_t LiveBvhOptions() {
	const auto value = LiveKnobs::Int("bvh_opt", -1);
	return value < 0 ? uint8_t {0} : static_cast<uint8_t>(0x80u | (static_cast<uint32_t>(value) & 0x1fu));
}

// KYTY_LIVE_KNOBS lane_branches=<0|1>: lane_exec_branches for compute programs compiled from now
// on (ShaderComputeInputInfo::lane_branch_live; the KYTY_LANE_BRANCHES_ONLY / _SKIP hash filters
// still apply); absent keeps KYTY_LANE_BRANCHES.
static uint8_t LiveLaneBranches() {
	const auto value = LiveKnobs::Int("lane_branches", -1);
	return value < 0 ? uint8_t {0} : value == 0 ? uint8_t {1} : uint8_t {2};
}

// KYTY_LIVE_KNOBS dead_lane=0/1: ShaderDeviceFeatures::dead_lane_fold for compute programs
// compiled from now on (ShaderComputeInputInfo::dead_lane_live); absent keeps KYTY_DEAD_LANE_FOLD.
static uint8_t LiveDeadLane() {
	const auto value = LiveKnobs::Int("dead_lane", -1);
	return value < 0 ? uint8_t {0} : value == 0 ? uint8_t {1} : uint8_t {2};
}

// KYTY_LIVE_KNOBS rt_half=0/1: the half-rate tile patch of shader/halfRateTiles.h for programs
// compiled from now on (ShaderComputeInputInfo::rt_half_live); absent keeps KYTY_RT_HALF.
static uint8_t LiveRtHalf() {
	const auto value = LiveKnobs::Int("rt_half", -1);
	return value < 0 ? uint8_t {0} : value == 0 ? uint8_t {1} : value == 2 ? uint8_t {3} : uint8_t {2};
}

// rt_half=2 needs pass 2 (recorded for indirect dispatches only) and the re-trace buffer; without
// them the adaptive variant would leave its edge pixels unwritten, so it falls back to rt_half=1.
static uint8_t RtHalfFor(uint8_t live, bool two_pass_possible) {
	const bool adaptive = live == 3u || (live == 0u && HalfRateTiles::EnvMode() == HalfRateTiles::kModeAdaptive);
	return adaptive && !two_pass_possible ? uint8_t {2} : live;
}

// KYTY_LIVE_KNOBS rt_fill=4|9|0|99: the half-rate fill mode (shader/halfRateTiles.h).
static uint8_t LiveRtFill() {
	return HalfRateTiles::FillModeFromKnob(LiveKnobs::Int("rt_fill", 4));
}

static bool ComputeGroupPackCandidate(const ShaderComputeInputInfo& input_info, uint32_t threads,
                                      uint32_t groups_x, bool turnip) {
	return ComputeGroupPackEnabled(turnip) && !input_info.dispatch_thread_dimensions && threads <= 32u &&
	       groups_x >= 2u && groups_x % 2u == 0u;
}

// KYTY_INDIRECT_DISPATCH_CLAMP=0 dispatches GPU-written indirect arguments unclamped (A/B).
static bool IndirectDispatchClampEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_INDIRECT_DISPATCH_CLAMP");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}


// The input info DispatchDirect compiles its program with (also built by the front for
// KYTY_GPU_PIPELINE, PrefetchDispatchDirect).
ShaderComputeInputInfo RenderExecutor::DirectDispatchInputInfo(const HW::ComputeShaderInfo& cs_regs,
                                                               uint32_t thread_group_x,
                                                               uint32_t thread_group_y,
                                                               uint32_t thread_group_z,
                                                               uint32_t mode, bool async_compute) {
	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	input_info.bvh_live      = LiveBvhOptions();
	input_info.lane_branch_live = LiveLaneBranches();
	input_info.dead_lane_live = LiveDeadLane();
	input_info.rt_half_live   = RtHalfFor(LiveRtHalf(), false); // direct dispatches have no pass 2
	input_info.rt_fill_live   = LiveRtFill();
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	input_info.workgroup_counts[0] = thread_group_x;
	input_info.workgroup_counts[1] = thread_group_y;
	input_info.workgroup_counts[2] = thread_group_z;
	if (use_thread_dimensions) {
		const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                                cs_regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size = std::max(group_sizes[axis], 1u);
			const auto threads = input_info.workgroup_counts[axis];
			input_info.workgroup_counts[axis] = threads / size + (threads % size != 0u);
		}
	}
	{
		const uint32_t threads = std::max(cs_regs.cs_regs.num_thread_x, 1u) *
		                         std::max(cs_regs.cs_regs.num_thread_y, 1u) *
		                         std::max(cs_regs.cs_regs.num_thread_z, 1u);
		if (ComputeGroupPackCandidate(input_info, threads, thread_group_x,
		                              m_context.GetGraphics().IsMesaTurnip())) {
			input_info.pack_groups = 2;
		}
	}
	return input_info;
}

// KYTY_GPU_PIPELINE front: everything DispatchDirect does before recording that reads guest
// memory through the SRT walker, from the live registers.
GpuPipeline::Prefetch RenderExecutor::PrefetchDispatchDirect(
    CommandBuffer& buffer, uint32_t thread_group_x, uint32_t thread_group_y,
    uint32_t thread_group_z, uint32_t mode, bool async_compute,
    GpuPipeline::PreparedComputeProgram& out) {
	const auto& sh_ctx = buffer.GetShaders();
	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0 ||
	    sh_ctx.GetCs().cs_regs.data_addr == 0) {
		return GpuPipeline::Prefetch::NoWalk; // DispatchDirect returns before its program lookup
	}
	const auto& cs_regs = sh_ctx.GetCs();
	out.input_info = DirectDispatchInputInfo(cs_regs, thread_group_x, thread_group_y,
	                                         thread_group_z, mode, async_compute);
	PipelineProbe::BeginItem();
	out.program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), out.input_info);
	NoteComputeItem(out.input_info);
	out.copies.Own(out.input_info.stage);
	return GpuPipeline::Prefetch::Prepared;
}

static bool ReadDsGpuWordRanges(RenderContext& context, vk::Buffer source,
                               std::span<vk::BufferCopy> copies, std::span<uint32_t> words,
                               bool preserve_bindings = false) {
	if (!source || copies.empty() || words.empty()) return false;
	auto& scheduler = context.GetCommandScheduler();
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	const auto size = words.size_bytes();
	auto [mapped, dest] = download.Map(size, 16u, true);
	if (!mapped) return false;
	download.Commit();
	scheduler.EndRendering(PassEnd::Buffer);
	const auto command = scheduler.Current().Handle();
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 1, &before,
	                        0, nullptr, 0, nullptr);
	for (auto& copy: copies) copy.dstOffset += dest;
	command.copyBuffer(source, download.Handle(), static_cast<uint32_t>(copies.size()), copies.data());
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eHost, {}, 1, &after,
	                        0, nullptr, 0, nullptr);
	// The CP producer capture runs inside a prepared dispatch. Finish also
	// executes deferred fault/cache callbacks, which can replace its bound
	// buffers. Wait completes the copy and keeps cleanup at the next GPU
	// operation boundary; legacy diagnostic callers retain their behavior.
	if (preserve_bindings) scheduler.Wait(scheduler.CurrentTick());
	else scheduler.Finish();
	download.Invalidate(dest, size);
	auto* output = reinterpret_cast<uint8_t*>(words.data());
	for (auto& copy: copies) {
		copy.dstOffset -= dest;
		std::memcpy(output + copy.dstOffset, mapped + copy.dstOffset, copy.size);
	}
	return true;
}

static bool ReadDsGpuWords(RenderContext& context, vk::Buffer source, uint64_t offset,
                           std::span<uint32_t> words, bool preserve_bindings = false) {
	vk::BufferCopy copy {offset, 0, words.size_bytes()};
	return ReadDsGpuWordRanges(context, source, std::span {&copy, 1}, words, preserve_bindings);
}

static std::vector<uint32_t> ReadDsBoundWords(RenderContext& context,
                                            const PreparedBindings& bindings,
                                            uint32_t slot, size_t max_words);

namespace {

constexpr uint64_t DsCoefficientWriter = 0x2f493aadc660cf03ull;
constexpr uint64_t DsCoefficientBytes = DemonsLightingMemoryPolicy::ProbeCoefficientBytes;
constexpr uint64_t DsProbeProjector = 0x567d21ad45386c8dull;
constexpr uint64_t DsRadianceRelight = 0xbcbcc6ba1e68c974ull;
constexpr uint32_t DsRadianceExtent = 256;

struct DsProbeSourceWrite {
	uint64_t address;
	uint64_t serial;
	std::vector<uint32_t> words;
	nlohmann::json dispatch;
	std::vector<uint64_t> radiance;
};

struct DsCoefficientDispatch {
	int32_t frame;
	uint64_t submit;
	uint64_t address;
	uint32_t slot;
	size_t writer_begin;
	size_t writer_sequence = 0;
	std::vector<uint32_t> before;
	std::vector<uint32_t> after;
	std::vector<uint32_t> source_coefficients;
	std::vector<uint32_t> source_weights;
	nlohmann::json inputs;
	std::shared_ptr<const DsProbeSourceWrite> source_write;
};

// These hooks run under the renderer context lock, like ds_buffer_producers.
std::shared_ptr<const DsCoefficientDispatch> ds_coefficient_dispatch;
// Keep several writers from the same guest frame: the atlas can consume
// separate 256/500-record updates before the frame counter advances.
std::array<std::shared_ptr<const DsCoefficientDispatch>, 8> ds_coefficient_snapshots {};
size_t ds_next_coefficient_snapshot = 0;
int32_t ds_last_coefficient_pair = -1;
std::array<std::shared_ptr<const DsProbeSourceWrite>, 4> ds_probe_source_snapshots;
size_t ds_next_probe_snapshot = 0;
struct DsRadianceWriterSample {
	int32_t frame = -1;
	uint32_t attempts = 0;
	nlohmann::json dispatch;
};
DsRadianceWriterSample ds_radiance_writer;
struct DsRelightMaskSample {
	int32_t frame = -1;
	uint64_t submit = 0;
	uint64_t address = 0;
	std::vector<uint32_t> words;
	nlohmann::json dispatch;
};
DsRelightMaskSample ds_relight_mask;
static std::array<DsRelightMaskSample,8> ds_transition_masks;
static size_t ds_transition_mask_next=0;
static bool DsTransitionEnabled(){return LibKernel::DemonsFileReadHistory::Enabled();}
static bool DsExtendedRayHistoryEnabled(){static const bool enabled=[] {const char* v=std::getenv("KYTY_DS_RAY_SOURCE_HISTORY");return v&&v[0]=='1';}();return enabled;}

// Producer activity is tracked independently of the bounded GPU snapshots.
// A missing post-transition ray file then has an unambiguous explanation:
// either the producer stopped submitting, or the capture gate did not fire.
struct DsRayProducerActivity {
	uint64_t observed = 0;
	uint64_t valid = 0;
	int32_t last_frame = -1;
	uint64_t last_submit = 0;
	uint64_t last_tick = 0;
};
DsRayProducerActivity ds_ray_producer_activity;
static DemonsRaySourceHistory::History ds_ray_source_history;
struct DsRaySourceDispatch {
    uint32_t id=0;
    std::vector<LibKernel::DemonsFileReadHistory::Event> file_reads;
    Cpu::DemonsGuestWriteHistory::Result write_faults;
};
static std::array<DsRaySourceDispatch,128> ds_ray_source_dispatches;

// The consumer often selects a probe long after its scatter upload. Record
// every uniquely mapped row of every observed producer, independent of the
// consumer's current selection. This is diagnostic-only, bounded to 128
// dispatches and 8 MiB per GPU readback. A failed/unknown update invalidates
// history, rather than reporting an older producer as the latest one.
static void RememberDsRaySourceRows(RenderContext& context,const PreparedBindings& bindings,
    uint64_t submit,std::span<const uint32_t> map,uint32_t base,uint32_t count) {
    namespace H=DemonsRaySourceHistory;
    auto& history=ds_ray_source_history;
    const uint32_t dispatch=static_cast<uint32_t>(ds_ray_producer_activity.valid);
    if(!dispatch||dispatch>128||map.empty()||base>=map.size()||count>map.size()-base||
        bindings.buffers.size()<3||bindings.buffer_sources.size()<3){history.Invalidate();return;}
    const auto& input=bindings.buffer_sources[1];const auto& output=bindings.buffer_sources[2];
    const auto& in=bindings.buffers[1];const auto& out=bindings.buffers[2];
    const auto off=bindings.runtime->program->bindings.memory_offset_dword;
    if(off>=bindings.shader_data.size()||!input.address||!output.address||
        !in.buffer||!out.buffer||in.range==VK_WHOLE_SIZE||out.range==VK_WHOLE_SIZE||
        input.address>UINT64_MAX-input.size||output.address>UINT64_MAX-output.size||
        (input.address<output.address+output.size&&output.address<input.address+input.size)){
        history.Invalidate();return;
    }
    const auto adjust_in=(bindings.shader_data[off]>>8u)&255u;
    const auto adjust_out=(bindings.shader_data[off]>>16u)&255u;
    if(adjust_in%4u||adjust_out%4u||adjust_in>in.range||adjust_out>out.range){
        history.Invalidate();return;
    }
    const uint64_t begin=uint64_t{base}*H::BytesPerProbe,end=uint64_t{base+count}*H::BytesPerProbe;
    if(end>input.size||end>in.range-adjust_in){history.Invalidate();return;}
    auto& provenance=ds_ray_source_dispatches[dispatch-1];
    provenance.id=dispatch;
    provenance.file_reads=LibKernel::DemonsFileReadHistory::Overlap(input.address+begin,end-begin);
    provenance.write_faults=Cpu::DemonsGuestWriteHistory::Overlap(input.address+begin,end-begin);
    history.Begin(output.address);
    std::vector<uint32_t> occurrence(H::ProbeCount);
    for(uint32_t row=base;row<base+count;++row){
        const auto probe=H::Destination(map[row]);
        if(probe!=H::NoRow)++occurrence[probe];
    }
    const auto frame=context.GetGpu().GetFrameNum();
    const auto tick=context.GetCommandScheduler().CurrentTick();
    uint32_t captured=0,different=0,backing_known=0,backing_diff=0,ambiguous=0;
    constexpr uint32_t batch=4096;
    for(uint32_t row=base;row<base+count;){
        const auto n=std::min(batch,base+count-row);
        const uint64_t byte=uint64_t{row}*H::BytesPerProbe;
        std::vector<uint32_t> source(size_t{n}*H::RowWords),target(source.size()),backing(source.size());
        if(!ReadDsGpuWords(context,in.buffer,in.offset+adjust_in+byte,source)){
            history.Invalidate();return;
        }
        std::vector<vk::BufferCopy> copies;
        for(uint32_t i=0;i<n;++i){
            const auto probe=H::Destination(map[row+i]);
            if(probe==H::NoRow)continue;
            const auto dst=uint64_t{probe}*H::BytesPerProbe;
            if(occurrence[probe]!=1){
                H::Record invalid;invalid.frame=frame;invalid.dispatch=dispatch;
                history.Put(probe,invalid);++ambiguous;continue;
            }
            if(dst>output.size||H::BytesPerProbe>output.size-dst||
                dst>out.range-adjust_out||H::BytesPerProbe>out.range-adjust_out-dst){
                history.Invalidate();return;
            }
            copies.emplace_back(out.offset+adjust_out+dst,uint64_t{i}*H::BytesPerProbe,H::BytesPerProbe);
        }
        if(!copies.empty()&&!ReadDsGpuWordRanges(context,out.buffer,copies,target)){
            history.Invalidate();return;
        }
        const bool have_backing=LibKernel::Memory::TryReadBacking(input.address+byte,backing.data(),
            backing.size()*sizeof(uint32_t));
        for(uint32_t i=0;i<n;++i){
            const auto probe=H::Destination(map[row+i]);
            if(probe==H::NoRow||occurrence[probe]!=1)continue;
            const auto src=std::span{source}.subspan(size_t{i}*H::RowWords,H::RowWords);
            const auto dst=std::span{target}.subspan(size_t{i}*H::RowWords,H::RowWords);
            H::Record record;record.frame=frame;record.submit=submit;record.tick=tick;
            record.source_guest=input.address+uint64_t{row+i}*H::BytesPerProbe;
            record.source_row=row+i;record.map_word=map[row+i];record.dispatch=dispatch;
            record.input=H::Summarize(src);record.output=H::Summarize(dst);
            record.input_output_diff=H::DifferentPairs(src,dst);
            if(have_backing){
                const auto b=std::span{backing}.subspan(size_t{i}*H::RowWords,H::RowWords);
                record.backing=H::Summarize(b);record.input_backing_diff=H::DifferentPairs(src,b);
                ++backing_known;backing_diff+=record.input_backing_diff!=0;
            }
            record.valid=true;history.Put(probe,record);++captured;different+=record.input_output_diff!=0;
        }
        row+=n;
    }
    Log::WriteToConsoleAndLog(fmt::format("[DS_RAY_SOURCE_HISTORY] frame={} submit={} dispatch={} captured={} different={} backing_known={} backing_different={} ambiguous={}\n",
        frame,submit,dispatch,captured,different,backing_known,backing_diff,ambiguous));
}

static nlohmann::json DsRaySourceAtConsumer(uint64_t owner,uint32_t probe,const nlohmann::json& ray_words){
    namespace H=DemonsRaySourceHistory;
    const auto* row=ds_ray_source_history.Find(owner,probe);
    if(!row)return {{"status","no_certified_producer"}};
    const auto summary=[](const H::Summary& s){return nlohmann::json{
        {"digest",fmt::format("{:016x}",s.digest)},{"colored_by_half",s.colored},
        {"high_nibble_by_half",s.high_nibble},{"finite_depth_by_half",s.finite_depth}};};
    nlohmann::json result={{"status","captured_producer"},{"frame",row->frame},{"submit",row->submit},
        {"tick",row->tick},{"source_guest",row->source_guest},{"source_row",row->source_row},
        {"map_word",row->map_word},{"input",summary(row->input)},{"output",summary(row->output)},
        {"input_output_different_pairs",row->input_output_diff}};
    if(row->input_backing_diff!=UINT32_MAX){
        result["backing_at_capture"]=summary(row->backing);
        result["input_backing_different_pairs"]=row->input_backing_diff;
    }
    if(ray_words.is_array()&&ray_words.size()==H::RowWords){
        const auto current=ray_words.get<std::vector<uint32_t>>();
        const auto s=H::Summarize(current);
        result["consumer_digest"]=fmt::format("{:016x}",s.digest);
        result["consumer_matches_producer_digest"]=s.digest==row->output.digest;
    }
    auto reads=nlohmann::json::array();
    if(row->dispatch&&row->dispatch<=ds_ray_source_dispatches.size()){
        const auto& p=ds_ray_source_dispatches[row->dispatch-1];
        if(p.id==row->dispatch)for(const auto& e:p.file_reads){
            if(e.begin<row->source_guest+H::BytesPerProbe&&e.end>row->source_guest)
                reads.push_back({{"event",e.id},{"path",e.path},{"begin",e.begin},{"end",e.end},
                    {"file_offset",e.file_offset},{"covers_row",e.begin<=row->source_guest&&e.end>=row->source_guest+H::BytesPerProbe}});
        }
    }
    // An overlapping file read is only a candidate; it may have been followed
    // by a guest memcpy/decompression or another write before GPU upload.
    result["file_read_candidates"]=std::move(reads);
    auto faults=nlohmann::json::array();
    if(row->dispatch&&row->dispatch<=ds_ray_source_dispatches.size()){
        const auto& p=ds_ray_source_dispatches[row->dispatch-1];
        const auto addr=Kernel::AddressFold::Fold(row->source_guest);
        if(p.id==row->dispatch){
            result["write_fault_candidates_truncated"]=p.write_faults.truncated;
            for(const auto& e:p.write_faults.events){
                const auto window=e.address&~(Cpu::DemonsGuestWriteHistory::Window-1);
                if(window>=addr+H::BytesPerProbe||window+Cpu::DemonsGuestWriteHistory::Window<=addr)continue;
                faults.push_back({{"event",e.id},{"fault_address",e.address},{"guest_rip",e.rip},
                    {"rsi",e.rsi},{"rdi",e.rdi},{"rcx",e.rcx},{"inside_row",e.address>=addr&&e.address<addr+H::BytesPerProbe}});
                if(faults.size()==16)break;
            }
        }
    }
    result["guest_write_fault_candidates"]=std::move(faults);
    return result;
}


struct DsRayProducerTargets {
	// Probe 4445 corresponds to the actually sampled dark tile at
	// layer 17, texel (214,89). Unlike earlier arbitrary probe IDs, this
	// ties the ray record to a confirmed dark pixel in queue 33.
	std::array<uint32_t, 4> probes {4445u, 4444u, 4461u, 902u};
	int32_t consumer_frame = -1;
};
DsRayProducerTargets ds_ray_producer_targets;

bool DsCoefficientPairDue(int32_t frame) {
	return frame >= 0 && (ds_last_coefficient_pair < 0 || frame < ds_last_coefficient_pair ||
	                      int64_t{frame} - ds_last_coefficient_pair >= 240);
}

bool DsCoefficientCaptureDue(int32_t frame) {
	// Retain occasional baseline samples, but arm detailed recordings only
	// after the compositor observes a real, sustained fall in HDR scene light.
	// No fixed boot, cutscene or gameplay frame is assumed.
	return DsCoefficientPairDue(frame) || DemonsAdaptiveCapture::Current().CaptureDue(frame);
}

static std::array<uint32_t,2> ds_relight_trace_targets{UINT32_MAX,UINT32_MAX};
static nlohmann::json ReadDsMaskProbeRecords(RenderContext& context,
    const PreparedBindings& bindings, uint32_t slot, std::span<const uint32_t> indices, uint32_t stride);

// Source: guest relight PC 0x000c reads s[16:23] from the user-data root;
// PC 0x00a4 loads V# s[0:3] from s[16:17]+0xc30. Flat slots 20..23
// match descriptor ordinal 3 in every non-null 125653 snapshot.
// Raw backing is not necessarily authoritative when GPU publication is pending.
static nlohmann::json DsTransitionBacking(RenderContext& context,uint64_t addr,uint32_t dwords){
    if(!addr||!dwords||dwords>8||addr>UINT64_MAX-dwords*4u)return {{"status","invalid_range"}};
    std::vector<uint32_t> values(dwords);
    auto& cache=context.GetBufferCache();
    const bool dirty=cache.HasGpuDirtyBytes(addr,dwords*4u),pending=cache.HasPendingBackingPublication(addr,dwords*4u);
    nlohmann::json j={{"address",addr},{"gpu_dirty",dirty},{"pending_publication",pending}};
    if(LibKernel::Memory::TryReadBacking(addr,values.data(),values.size()*4))j["words"]=values;
    else j["status"]="backing_unavailable";
    auto history=nlohmann::json::array();
    for(const auto& e:cache.RecentProbeRayTransfers(addr,dwords*4u))if(e.path)
        history.push_back({{"path",e.path},{"tick",e.tick},{"source",e.source},{"shader",fmt::format("{:016x}",e.shader)},{"begin",e.begin},{"end",e.end}});
    j["transfer_candidates"]=std::move(history);
    if(const auto* w=FindDsBufferProducer(context.GetGpu().GetFrameNum(),addr,dwords*4u,false))
        j["compute_writer_candidate"]={{"shader",fmt::format("{:016x}",w->shader)},{"frame",w->frame},{"submit",w->submit},{"address",w->address},{"size",w->size}};
    return j;
}
static nlohmann::json DsDescriptor224(RenderContext& context,const PreparedBindings& bindings){
    namespace T=DemonsTransitionTrace;
    const auto& s=*bindings.runtime->resources;const auto& p=*bindings.runtime->program;
    if(s.buffers.size()!=9||s.flattened_srt.size()!=174||p.info.buffers.size()!=9)
        return {{"status","unexpected_layout"}};
    const auto& flat=s.flattened_srt;const auto& d=s.buffers[3];
    const auto decoded=DecodeNativeDescriptor<ShaderBufferResource>(d);
    if(decoded.Base48()&&(decoded.Stride()!=384||decoded.NumRecords()!=224))
        return {{"status","unexpected_descriptor"}};
    // Guest 0x0c loads s[16:23] from user data; 0xa4 loads the V# at s[16:17]+0xc30.
    // The flattened pointer is at 2:3, not 0:1 (all eight captured root-table reads agree).
    const uint64_t root=T::Pointer(flat[2],flat[3]),address=T::Descriptor224Address(root);
    const uint64_t old_candidate=T::Pointer(flat[0],flat[1]);
    const auto& tr=s.ds_transition_descriptor;
    nlohmann::json j={{"status","captured"},{"null",decoded.Base48()==0},{"root",root},{"address",address},
        {"flat_root_slots",{2,3}},{"old_flat_root_candidate",old_candidate},
        {"flat_words",std::vector<uint32_t>(flat.begin()+20,flat.begin()+24)},{"materialized",d.dwords},
        {"raw_descriptor_captured",tr.captured},{"source_active",tr.active},{"source",tr.source},
        {"normalized_to_null",tr.normalized},{"raw_descriptor",tr.raw.dwords}};
    if(s.user_data.size()>=2){
        const auto table=T::Pointer(s.user_data[0],s.user_data[1]);
        j["root_table_backing"]=DsTransitionBacking(context,table,8);
        const auto& tb=j["root_table_backing"];
        if(tb.contains("words")&&tb["words"].size()==8){
            const auto raw=tb["words"].get<std::array<uint32_t,8>>();
            const auto backing_root=T::Pointer(raw[0],raw[1]);
            j["root_table_first_pointer"]=backing_root;
            j["root_matches_table_backing"]=root==backing_root;
            j["root_backing_may_be_stale"]=tb.value("gpu_dirty",true)||tb.value("pending_publication",true);
            if(backing_root!=root&&backing_root)
                j["table_chain_descriptor_backing"]=DsTransitionBacking(context,T::Descriptor224Address(backing_root),4);
        }
    }
    j["descriptor_backing"]=DsTransitionBacking(context,address,4);
    const auto& backing=j["descriptor_backing"];
    if(backing.contains("words")&&backing["words"].size()==4){
        const auto actual=backing["words"].get<std::array<uint32_t,4>>();
        j["backing_matches_flat"]=std::equal(actual.begin(),actual.end(),flat.begin()+20);
        j["backing_matches_materialized"]=std::equal(actual.begin(),actual.end(),d.dwords.begin());
        j["backing_may_be_stale"]=backing.value("gpu_dirty",true)||backing.value("pending_publication",true);
        j["backing_chain_verified"]=j.value("root_matches_table_backing",false)&&!j.value("root_backing_may_be_stale",true)&&!j.value("backing_may_be_stale",true);
    }
    return j;
}
struct DsBrightMaskRecord {int32_t frame=-1;std::array<uint32_t,4> position{};std::array<uint32_t,16> mask{};};
static std::vector<DsBrightMaskRecord> ds_bright_mask_records;
static nlohmann::json DsMaskTransition(RenderContext& context,const PreparedBindings& bindings,
    std::span<const uint32_t> ids,bool bright_reference,uint32_t& matched_lost_index){
    namespace T=DemonsTransitionTrace;
    const auto& p=*bindings.runtime->program;const auto frame=context.GetGpu().GetFrameNum();
    const auto slot=p.info.buffers[4].descriptor_index;
    if(slot>=bindings.buffer_sources.size())return {{"status","missing_mask"}};
    const auto words=ReadDsBoundWords(context,bindings,slot,ids.size()*16);
    if(words.size()!=ids.size()*16)return {{"status","mask_read_failed"}};
    const auto c=T::Summarize(words,ids);
    nlohmann::json result={{"status","captured"},{"rows",c.rows},{"valid_rows",c.valid},
        {"group1_rows",c.group1},{"group1_light_bits",c.group1_lights},{"inconsistent_group1",c.inconsistent},
        {"mask_words_all",words},{"probe_ids_all",std::vector<uint32_t>(ids.begin(),ids.end())}};
    const auto& source=bindings.buffer_sources[slot];
    if(const auto* w=FindDsBufferProducer(frame,source.address,words.size()*4,false)){
        result["last_writer"]={{"shader",fmt::format("{:016x}",w->shader)},{"frame",w->frame},{"submit",w->submit}};
        for(const auto& x:ds_transition_masks){
            if(x.frame!=w->frame||x.submit!=w->submit||x.address!=source.address||w->shader!=DemonsRadianceTrace::MaskWriter)continue;
            result["writer_words_match"]=T::ExactWriter(words,x.words);
            result["writer_inputs"]=x.dispatch;
            result["writer_output_words"]=x.words;
            break;
        }
    }
    // Match the same probe AND its same position, not arbitrary min/max rows in
    // a different scene. Compare the previous bright mask before updating it.
    if(ds_bright_mask_records.empty())ds_bright_mask_records.resize(92160);
    const auto positions=ReadDsMaskProbeRecords(context,bindings,p.info.buffers[2].descriptor_index,ids,16);
    auto matches=nlohmann::json::array();uint32_t lost=0,matched=0;
    if(positions.is_array()&&positions.size()==ids.size()){
        for(uint32_t i=0;i<ids.size();++i){
            const auto row=std::span{words}.subspan(i*16,16);
            if(ids[i]>=ds_bright_mask_records.size()||!T::ValidRow(row,ids[i])||!positions[i].is_array()||positions[i].size()!=4)continue;
            const auto pos=positions[i].get<std::array<uint32_t,4>>();
            auto& old=ds_bright_mask_records[ids[i]];
            if(old.frame>=0&&old.frame<frame&&old.position==pos){
                ++matched;const bool loss=(old.mask[13]&2u)&&!(row[13]&2u);
                if(loss){++lost;if(matched_lost_index==UINT32_MAX)matched_lost_index=i;}
                if(matches.size()<64||loss)matches.push_back({{"id",ids[i]},{"z",i},{"position",pos},{"bright_frame",old.frame},
                    {"bright_mask",old.mask},{"current_mask",std::vector<uint32_t>(row.begin(),row.end())},{"lost_group1",loss}});
            }
            if(bright_reference&&(row[13]&2u)){old.frame=frame;old.position=pos;std::copy(row.begin(),row.end(),old.mask.begin());}
        }
    }
    result["matched_bright_positions"]=matched;result["lost_group1_same_position"]=lost;
    result["bright_matches"]=std::move(matches);
    return result;
}
static void SaveDsTransitionEdge(int32_t frame,uint64_t submit,uint32_t phase,const nlohmann::json& report){
    const auto folder=Config::GetShaderDumpFolder()/"queue-state";
    std::error_code ec;std::filesystem::create_directories(folder,ec);
    std::ofstream file(folder/fmt::format("ds_transition_edge_f{}_s{}_p{}.json",frame,submit,phase),std::ios::trunc);
    file<<report;
}

struct DsRelightSelection {
    std::array<uint32_t,3> header{0,UINT32_MAX,UINT32_MAX};
    std::array<uint32_t,2> probes{UINT32_MAX,UINT32_MAX};
    std::array<uint32_t,3> groups{};
    uint32_t phase=0;
    nlohmann::json transition;
};
static DsRelightSelection SelectDsRelightTrace(RenderContext& context,const PreparedBindings& bindings,
    std::array<uint32_t,3> groups,uint64_t submit,vk::Buffer indirect={},uint64_t offset=0){
    namespace P=DemonsRelightProbe;
    DsRelightSelection result;
    const auto& p=*bindings.runtime->program;
    if(p.shader_hash!=P::Shader||p.info.group_pack>1)return result;
    const bool focus=DsTransitionEnabled();
    if(!focus&&!p.ds_light_math_variant)return result;
    const auto frame=context.GetGpu().GetFrameNum();
    const auto& gate=DemonsAdaptiveCapture::Current();
    const uint32_t phase=gate.PostTransitionDarkDue(frame)?3:gate.DarkOnsetDue(frame)?2:gate.BrightSceneActive(frame)?1:0;
    static std::array<int32_t,4> last{-10000,-10000,-10000,-10000};
    static std::array<uint32_t,4> count{};
    const bool ordinary_due=count[phase]<(phase==3?3u:2u)&&int64_t{frame}-last[phase]>=24;
    if(!focus&&!ordinary_due)return result;
    if(indirect&&!ReadDsGpuWords(context,indirect,offset,groups))return result;
    if(groups[0]!=2||groups[1]!=2||!groups[2]||groups[2]>256)return result;
    if(p.info.buffers.size()<6)return result;
    const auto map_slot=p.info.buffers[0].descriptor_index;
    const auto ray_slot=p.info.buffers[1].descriptor_index;
    auto ids=ReadDsBoundWords(context,bindings,map_slot,groups[2]);
    if(ids.size()!=groups[2])return result;
    uint32_t lost_index=UINT32_MAX;
    bool edge=false;
    if(focus){
        auto desc=DsDescriptor224(context,bindings);
        const bool known=desc.value("status",std::string{})=="captured";
        const bool is_null=desc.value("null",false);
        auto mask=DsMaskTransition(context,bindings,ids,phase==1&&known&&!is_null,lost_index);
        static int previous=-1;static uint32_t edge_count=0;static bool had_bright=false;
        if(phase==1)had_bright=true;
        if(known){edge=had_bright&&previous>=0&&previous!=int(is_null)&&edge_count<8;previous=int(is_null);}
        const auto valid=mask.value("valid_rows",0u),group1=mask.value("group1_rows",0u),lost=mask.value("lost_group1_same_position",0u);
        Log::WriteToConsoleAndLog(fmt::format("[DS_TRANSITION] frame={} submit={} phase={} descriptor_known={} descriptor_null={} mask_valid={} group1_rows={} matched_lost={} edge={}\n",frame,submit,phase,known,is_null,valid,group1,lost,edge));
        result.transition={{"frame",frame},{"submit",submit},{"phase",phase},{"descriptor_224",std::move(desc)},{"mask",std::move(mask)}};
        if(edge){++edge_count;SaveDsTransitionEdge(frame,submit,phase,result.transition);}
    }
    if(!ordinary_due||!p.ds_light_math_variant)return result;
    uint32_t first=0,second=groups[2]/2;
    // Within one dispatch, pair the lowest and highest nonzero emission/
    // color scores, rather than unrelated probes at arbitrary Z positions.
    // Record the actual GPU buffer, never infer a probe's contents from ID.
    const auto records=ReadDsMaskProbeRecords(context,bindings,ray_slot,ids,2048);
    // Compare the exact same probe and position across captured dispatches, including
    // two late gameplay dispatches with different radiance. Do not conflate different scenes.
    struct RayReference {
        int32_t frame=-1;
        std::array<uint32_t,4> position{};
        uint32_t color=0,emission=0;
        std::array<uint32_t,512> words{};
    };
    static std::unordered_map<uint32_t,RayReference> references;
    static int32_t previous_frame=-1;
    if(previous_frame>frame) references.clear();
    previous_frame=frame;
    const auto positions=ReadDsMaskProbeRecords(context,bindings,p.info.buffers[2].descriptor_index,ids,16);
    auto pairs=nlohmann::json::array();
    uint32_t ray_loss_index=UINT32_MAX, max_loss=0, matched=0;
    nlohmann::json strongest;
    if(records.is_array()&&records.size()==ids.size()){
        uint32_t lo=UINT32_MAX,hi=0;bool found=false;
        for(uint32_t i=0;i<ids.size();++i){
            const auto& row=records[i];if(!row.is_array()||row.size()!=512)continue;
            uint32_t color=0,emission=0;
            for(uint32_t j=0;j<512;j+=2){
                const auto v=row[j].get<uint32_t>();
                color+=(v&0xffffffu)!=0;emission+=(v>>28)!=0;
            }
            const uint32_t score=emission*257u+color;
            if(!found||score<lo){lo=score;first=i;}
            if(!found||score>hi){hi=score;second=i;}
            found=true;
            if(ids[i]>=92160u||!positions.is_array()||positions.size()!=ids.size()||
                !positions[i].is_array()||positions[i].size()!=4)continue;
            const auto pos=positions[i].get<std::array<uint32_t,4>>();
            if(references.size()>=4096u&&!references.contains(ids[i]))continue;
            auto& ref=references[ids[i]];
            if(ref.frame>=0&&ref.frame<frame&&ref.position==pos){
                ++matched;
                const uint32_t loss=ref.color>color?ref.color-color:0u;
                if(loss){
                    pairs.push_back({{"id",ids[i]},{"z",i},{"position",pos},{"reference_frame",ref.frame},
                        {"reference_color",ref.color},{"current_color",color},
                        {"reference_emission",ref.emission},{"current_emission",emission}});
                    if(loss>max_loss){
                        max_loss=loss;ray_loss_index=i;
                        strongest=pairs.back();
                        strongest["reference_words"]=ref.words;
                        strongest["current_words"]=row;
                    }
                }
            }
            // Retain the highest-color known observation for this exact position.
            // This is a data reference, not a claim that the frame was illuminated.
            if(ref.frame<0||ref.position!=pos||color>ref.color){
                ref.frame=frame;ref.position=pos;ref.color=color;ref.emission=emission;
                ref.words=row.get<std::array<uint32_t,512>>();
            }
        }
    }
    result.transition["matched_ray_records"]={{"matched",matched},{"color_losses",pairs},
        {"strongest_loss",strongest},{"reference_policy","same_id_exact_position_highest_color"}};
    if(ray_loss_index!=UINT32_MAX)first=ray_loss_index;
    else if(lost_index!=UINT32_MAX)first=lost_index;
    if(second==first&&groups[2]>1)second=(first+1)%groups[2];
    result.header={1,first,second==first?UINT32_MAX:second};
    result.probes={ids[first],second==first?UINT32_MAX:ids[second]};
    result.groups=groups;result.phase=phase;last[phase]=frame;++count[phase];
    if(focus&&p.info.buffers.size()==9&&bindings.runtime->resources->buffers.size()==9){
        const auto& d=bindings.runtime->resources->buffers[3];const auto desc=DecodeNativeDescriptor<ShaderBufferResource>(d);
        if(desc.Base48()&&desc.Stride()==384&&desc.NumRecords()==224)
            result.transition["descriptor_224_records_before_dispatch"]=ReadDsBoundWords(context,bindings,p.info.buffers[3].descriptor_index,224u*96u);
    }
    ds_relight_trace_targets=result.probes;
    return result;
}

void RetagDsRecordedComputeWrites(RenderContext& context, const PreparedBindings& bindings,
                                  uint64_t preparation_tick) {
	auto& cache = context.GetBufferCache();
	if (!BufferCache::ShaderWriteRetickEnabled()) return;
	for (const auto& write : bindings.runtime->program->info.buffers) {
		if (!write.written || write.descriptor_index >= bindings.buffer_sources.size()) continue;
		const auto& source = bindings.buffer_sources[write.descriptor_index];
		if (source.address && source.size)
			cache.RetagShaderWrite(source.address, source.size, preparation_tick);
	}
}

const DsBufferProducer* FindDsProbeSourceWriter(int32_t frame, uint64_t address, uint64_t bytes) {
	if (!address || !bytes) return nullptr;
	if (const auto* latest = FindDsBufferProducer(frame, address, bytes)) return latest;
	for (size_t i = 0; i < ds_probe_buffer_writes.size(); ++i) {
		const auto index = (ds_next_probe_write + ds_probe_buffer_writes.size() - 1 - i) %
		                   ds_probe_buffer_writes.size();
		const auto& writer = ds_probe_buffer_writes[index];
		if (!writer.size || writer.frame > frame) continue;
		if (address >= writer.address ? address - writer.address < writer.size
		                              : writer.address - address < bytes) return &writer;
	}
	return nullptr;
}

static nlohmann::json ReadDsMaskProbeRecords(RenderContext& context,
                                           const PreparedBindings& bindings, uint32_t slot,
                                           std::span<const uint32_t> indices, uint32_t stride) {
	if (slot >= bindings.buffers.size() || slot >= bindings.buffer_sources.size()) return nullptr;
	const auto& native = bindings.buffers[slot];
	const auto& source = bindings.buffer_sources[slot];
	const auto offset_word = bindings.runtime->program->bindings.memory_offset_dword + slot / 4u;
	if (!native.buffer || native.range == VK_WHOLE_SIZE ||
	    offset_word >= bindings.shader_data.size()) return nullptr;
	const uint64_t adjustment = (bindings.shader_data[offset_word] >> (slot % 4u * 8u)) & 255u;
	const auto words_per_record = stride / sizeof(uint32_t);
	std::vector<uint32_t> words(indices.size() * words_per_record);
	std::vector<vk::BufferCopy> copies;
	std::vector<bool> valid(indices.size());
	for (size_t i = 0; i < indices.size(); ++i) {
		const uint64_t guest = uint64_t{indices[i]} * stride;
		const uint64_t begin = adjustment + guest;
		if (guest > source.size || stride > source.size - guest ||
		    begin > native.range || stride > native.range - begin) continue;
		copies.emplace_back(native.offset + begin, i * stride, stride);
		valid[i] = true;
	}
	if (!ReadDsGpuWordRanges(context, native.buffer, copies, words)) return nullptr;
	auto records = nlohmann::json::array();
	for (size_t i = 0; i < indices.size(); ++i) {
		if (!valid[i]) {
			records.push_back(nullptr);
			continue;
		}
		const auto begin = words.begin() + i * words_per_record;
		records.push_back(std::vector<uint32_t>(begin, begin + words_per_record));
	}
	return records;
}

static DemonsAtlasTileHistory::History ds_atlas_tile_history;

// Capture the complete atlas update map, not a periodic subsample. The
// consuming relight shader often reads tiles last updated many frames ago.
static void RememberDsAtlasTileInputs(RenderContext& context,const PreparedBindings& bindings,
    uint64_t submit,DispatchGroups groups) {
    namespace H=DemonsAtlasTileHistory;
    auto& history=ds_atlas_tile_history;
    const auto& program=*bindings.runtime->program;
    const auto& srt=bindings.runtime->resources->flattened_srt;
    if(!groups[0]||!groups[1]||!groups[2])return;
    if(groups[0]!=2||groups[1]!=2||groups[2]>16384||program.info.group_pack>1||
        srt.size()<28||program.info.buffers.size()<2){history.Invalidate();return;}
    const auto map_slot=program.info.buffers[0].descriptor_index;
    const auto coeff_slot=program.info.buffers[1].descriptor_index;
    const auto& info=program.info.images;
    uint64_t owner=0,version=0;
    for(size_t i=0;i<info.size()&&i<bindings.images.size();++i){
        const auto& b=bindings.images[i];
        if(!info[i].written||!b.image_id||b.placeholder)continue;
        const auto& image=context.GetTextureCache().GetImage(b.image_id);
        const auto v=std::ranges::find(image.views,b.image_view,&CachedImageView::view);
        if(v==image.views.end()||image.backing.format!=vk::Format::eR16G16B16A16Sfloat||
            image.backing.extent.width!=H::Width||image.backing.extent.height!=H::Height||
            image.backing.layers!=H::Layers||v->info.base_level||v->info.base_layer||
            v->info.layer_count!=H::Layers)continue;
        if(owner){history.Invalidate();return;}
        owner=image.info.data.address;version=image.ContentVersion();
    }
    if(!owner||uint64_t{srt[27]}+groups[2]>92160){history.Invalidate();return;}
    const auto map=ReadDsBoundWords(context,bindings,map_slot,size_t{srt[27]}+groups[2]);
    if(map.size()!=size_t{srt[27]}+groups[2]||coeff_slot>=bindings.buffers.size()||
        coeff_slot>=bindings.buffer_sources.size()){history.Invalidate();return;}
    const auto& b=bindings.buffers[coeff_slot];
    const auto& source=bindings.buffer_sources[coeff_slot];
    const auto offword=program.bindings.memory_offset_dword+coeff_slot/4u;
    if(!b.buffer||b.range==VK_WHOLE_SIZE||offword>=bindings.shader_data.size()){
        history.Invalidate();return;
    }
    const uint64_t adjustment=(bindings.shader_data[offword]>>(8u*(coeff_slot%4u)))&255u;
    if(adjustment%4u){history.Invalidate();return;}
    std::array<uint32_t,6> layout{};
    std::copy_n(srt.begin()+22,6,layout.begin());
    std::vector<uint32_t> values(size_t{groups[2]}*14);
    std::vector<uint32_t> keys(groups[2],H::Invalid);
    std::vector<vk::BufferCopy> copies;
    for(uint32_t z=0;z<groups[2];++z){
        const auto raw=map[srt[27]+z];
        const auto key=H::IndexKey(raw,layout);
        const uint64_t off=uint64_t{raw&0xffffffu}*64u;
        if(key==H::Invalid||off>source.size||56u>source.size-off||
            adjustment>b.range||off>b.range-adjustment||56u>b.range-adjustment-off){
            // A dispatch with an unknown candidate cannot certify old tiles.
            history.Invalidate();return;
        }
        keys[z]=key;
        copies.emplace_back(b.offset+adjustment+off,uint64_t{z}*56u,56u);
    }
    if(!ReadDsGpuWordRanges(context,b.buffer,copies,values)){history.Invalidate();return;}
    history.Begin(owner,version);
    for(uint32_t z=0;z<groups[2];++z){
        H::Record record;
        record.frame=context.GetGpu().GetFrameNum();record.submit=submit;record.version=version;
        record.group_z=z;record.packed_index=map[srt[27]+z];
        std::copy_n(values.begin()+size_t{z}*14,14,record.coefficients.begin());
        history.Put(keys[z],record);
    }
}

static nlohmann::json DsAtlasFootprintProducers(uint64_t owner,uint64_t version,
    uint32_t x,uint32_t y,uint32_t layer){
    namespace H=DemonsAtlasTileHistory;
    auto result=nlohmann::json::array();
    for(uint32_t dy=0;dy<2;++dy)for(uint32_t dx=0;dx<2;++dx){
        const auto key=H::Key(x+dx,y+dy,layer);
        const auto* r=ds_atlas_tile_history.Find(owner,version,key);
        if(!r){result.push_back(nullptr);continue;}
        result.push_back({{"tile",key},{"frame",r->frame},{"submit",r->submit},
            {"version",r->version},{"z",r->group_z},{"packed_index",r->packed_index},
            {"ambiguous",r->ambiguous},{"coefficient_words",r->coefficients}});
    }
    return result;
}

// Site 12 in all four captured relight modules is a float 2D-array
// image sample (native binding 4, array element 2, explicit LOD 0).
// Read the four native texels of the very same sample. This separates
// pre-existing dark atlas content from a wrong image/sampler/coordinate.
static nlohmann::json ReadDsRelightAtlasFootprints(RenderContext& context,
    const PreparedBindings& bindings, std::span<const uint32_t> words) {
    namespace P=DemonsRelightProbe;
    namespace IR=ShaderRecompiler::IR;
    const auto& program=*bindings.runtime->program;
    // Resolve the actual compiled image instruction; fingerprint alone is not a resource identity.
    if(program.ds_relight_site12_binding!=4||program.ds_relight_site12_element!=2)
        return {{"status","different_or_unresolved_site12_resource"},{"binding",program.ds_relight_site12_binding},{"element",program.ds_relight_site12_element}};
    const auto& resources=program.info.images;
    size_t slot=resources.size();
    for(size_t i=0;i<resources.size();++i){
        const auto kind=IR::DescriptorBindingForImage(resources[i]);
        if(!kind || IR::NativeBinding(program.stage,*kind)!=4u || resources[i].descriptor_index!=2u)continue;
        if(slot!=resources.size())return {{"status","ambiguous_descriptor"}};
        slot=i;
    }
    if(slot>=resources.size()||slot>=bindings.images.size()||resources[slot].written)
        return {{"status","source_descriptor_missing_or_written"}};
    const auto& binding=bindings.images[slot];
    if(!binding.image_id||binding.placeholder)return {{"status","placeholder"}};
    auto& image=context.GetTextureCache().GetImage(binding.image_id);
    const auto found=std::ranges::find(image.views,binding.image_view,&CachedImageView::view);
    if(found==image.views.end())return {{"status","missing_view"}};
    const auto view=found->info;
    const auto& host=image.backing;
    nlohmann::json report={{"status","unsupported_view"},{"resource",slot},
        {"binding",4},{"element",2},{"guest",binding.desc.info.data.address},
        {"owner",image.info.data.address},{"version",image.ContentVersion()},
        {"format",static_cast<uint32_t>(host.format)},{"mip",view.base_level},
        {"layer",view.base_layer},{"layers",view.layer_count}};
    if(host.format!=vk::Format::eR16G16B16A16Sfloat||host.samples!=1||
        !(host.usage&vk::ImageUsageFlagBits::eTransferSrc)||view.type!=vk::ImageViewType::e2DArray||
        view.base_level>=host.mip_levels||view.base_layer>=host.layers)return report;
    // A post-dispatch copy is valid only when the sampled image did not
    // alias an output of the dispatch.
    for(size_t i=0;i<resources.size()&&i<bindings.images.size();++i){
        if(!resources[i].written||!bindings.images[i].image_id||bindings.images[i].placeholder)continue;
        const auto& output=context.GetTextureCache().GetImage(bindings.images[i].image_id);
        const auto& a=image.info.data;const auto& b=output.info.data;
        if(bindings.images[i].image_id==binding.image_id||
            (a.size&&b.size&&a.address<b.address+b.size&&b.address<a.address+a.size)){
            report["status"]="source_aliases_output";return report;
        }
    }
    const uint32_t width=std::max(host.extent.width>>view.base_level,1u);
    const uint32_t height=std::max(host.extent.height>>view.base_level,1u);
    report["extent"]={width,height};
    struct Sample{uint32_t probe,lane,event,x,y,layer;float u,v;std::array<uint32_t,4> rgba;};
    std::vector<Sample> samples;
    // Sample at most 32 unique footprints per probe, stratified over lanes.
    // Keep only interior LOD0 reads with integral in-range array indices.
    for(uint32_t p=0;p<P::Probes;++p){
        const auto start=samples.size();
        for(uint32_t lane=0;lane<P::Rays&&samples.size()-start<32;lane+=4){
            const auto off=P::Header+(p*P::Rays+lane)*P::Sites*P::SiteWords+12u*P::SiteWords;
            for(uint32_t event=0;event<std::min(words[off],P::Events)&&samples.size()-start<32;++event){
                const auto* a=words.data()+off+1+event*P::EventWords;
                const float u=std::bit_cast<float>(a[0]),v=std::bit_cast<float>(a[1]),z=std::bit_cast<float>(a[2]);
                if(a[8]!=0||!std::isfinite(u)||!std::isfinite(v)||!std::isfinite(z)||
                    u<=0||u>=1||v<=0||v>=1||z<0||z!=std::floor(z)||
                    z>=static_cast<float>(std::min(view.layer_count,host.layers-view.base_layer)))continue;
                const auto x=static_cast<int32_t>(std::floor(u*width-0.5f));
                const auto y=static_cast<int32_t>(std::floor(v*height-0.5f));
                if(x<0||y<0||uint32_t(x+1)>=width||uint32_t(y+1)>=height)continue;
                const auto layer=view.base_layer+static_cast<uint32_t>(z);
                if(std::ranges::any_of(samples,[&](const auto& q){return q.probe==p&&q.x==uint32_t(x)&&q.y==uint32_t(y)&&q.layer==layer;}))continue;
                samples.push_back({p,lane,event,uint32_t(x),uint32_t(y),layer,u,v,{a[4],a[5],a[6],a[7]}});
            }
        }
    }
    if(samples.empty()){report["status"]="no_interior_samples";return report;}
    constexpr uint32_t stride=32;
    const uint32_t bytes=static_cast<uint32_t>(samples.size())*stride;
    auto& download=context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped,offset]=download.Map(bytes,16u,true);
    if(!mapped){report["status"]="download_map_failed";return report;}
    download.Commit();
    std::vector<vk::BufferImageCopy> copies(samples.size());
    for(size_t i=0;i<samples.size();++i){
        const auto& sample=samples[i];auto& copy=copies[i];
        copy.bufferOffset=offset+i*stride;
        copy.imageSubresource.aspectMask=vk::ImageAspectFlagBits::eColor;
        copy.imageSubresource.mipLevel=view.base_level;
        copy.imageSubresource.baseArrayLayer=sample.layer;
        copy.imageSubresource.layerCount=1;
        copy.imageOffset=vk::Offset3D{static_cast<int32_t>(sample.x),static_cast<int32_t>(sample.y),0};
        copy.imageExtent=vk::Extent3D{2,2,1};
    }
    image.Download(std::span{copies},download.Handle(),offset,bytes);
    context.GetCommandScheduler().Finish();download.Invalidate(offset,bytes);
    auto data=nlohmann::json::array();
    for(size_t i=0;i<samples.size();++i){
        const auto& sample=samples[i];std::array<uint16_t,16> texels{};
        std::memcpy(texels.data(),mapped+i*stride,stride);
        data.push_back({{"probe_slot",sample.probe},{"lane",sample.lane},{"event",sample.event},
            {"xy",{sample.x,sample.y}},{"layer",sample.layer},{"uv",{sample.u,sample.v}},
            {"sample_rgba32_bits",sample.rgba},{"native_rgba16_bits",texels},
            {"atlas_writer_candidates",DsAtlasFootprintProducers(image.info.data.address,image.ContentVersion(),sample.x,sample.y,sample.layer)}});
    }
    report["status"]="captured";report["samples"]=std::move(data);
    return report;
}

static void SaveDsRelightTrace(RenderContext& context,const PreparedBindings& bindings,
    uint64_t submit,const DsRelightSelection& sel){
    namespace P=DemonsRelightProbe;
    if(!sel.header[0])return;
    std::vector<uint32_t> words(P::WordCount);
    if(!ReadDsGpuWords(context,bindings.shared_memory.buffer,bindings.shared_memory.offset,words))return;
    const auto frame=context.GetGpu().GetFrameNum();
    const auto folder=Config::GetShaderDumpFolder()/"queue-state";
    std::error_code ec;std::filesystem::create_directories(folder,ec);
    const auto stem=fmt::format("ds_relight_trace_f{}_s{}_p{}",frame,submit,sel.phase);
    std::ofstream out(folder/(stem+".bin"),std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(uint32_t));out.close();
    if(!out.good())return;
    const auto& snap=*bindings.runtime->resources;
    nlohmann::json desc=nlohmann::json::array(),images=nlohmann::json::array(),samplers=nlohmann::json::array();
    for(const auto& a:snap.buffers)desc.push_back(a.dwords);
    for(const auto& a:snap.images)images.push_back(a.dwords);
    for(const auto& a:snap.samplers)samplers.push_back(a.dwords);
    std::vector<uint32_t> probes,z;
    for(uint32_t i=0;i<P::Probes;++i)if(sel.probes[i]!=UINT32_MAX){probes.push_back(sel.probes[i]);z.push_back(sel.header[i+1]);}
    // Resource ordinal is not the native descriptor binding index.
    const auto& info=bindings.runtime->program->info.buffers;
    if(info.size()<6)return;
    const auto ray_slot=info[1].descriptor_index, position_slot=info[2].descriptor_index;
    const auto mask_slot=info[4].descriptor_index, light_slot=info[5].descriptor_index;
    const auto rays=ReadDsMaskProbeRecords(context,bindings,ray_slot,probes,2048);
    const auto masks=ReadDsMaskProbeRecords(context,bindings,mask_slot,z,64);
    const auto lights=ReadDsBoundWords(context,bindings,light_slot,384u*96u);
    const auto positions=ReadDsMaskProbeRecords(context,bindings,position_slot,probes,16);
    auto mask_checks=nlohmann::json::array();
    for(size_t i=0;i<probes.size();++i){
        const bool valid=masks.is_array()&&i<masks.size()&&masks[i].is_array()&&masks[i].size()==16;
        mask_checks.push_back(valid&&masks[i][14].get<uint32_t>()==probes[i]);
    }
    nlohmann::json report={{"frame",frame},{"submit",submit},{"phase",sel.phase},
        {"fingerprint",fmt::format("{:016x}",bindings.runtime->program->ds_light_math_variant)},
        {"groups",sel.groups},{"probe_ids",sel.probes},{"z",sel.header},
        {"header_words",P::Header},{"rays",P::Rays},{"sites",P::Sites},{"events",P::Events},
        {"site_words",P::SiteWords},{"event_words",P::EventWords},
        {"binary",stem+".bin"},{"ray_words",rays},{"mask_words",masks},{"light_words",lights},
        {"positions",positions},{"mask_probe_matches",mask_checks},
        {"native_slots",{{"ray",ray_slot},{"position",position_slot},{"mask",mask_slot},{"light",light_slot}}},
        {"srt",snap.flattened_srt},{"shader_data",bindings.shader_data},
        {"buffers",desc},{"images",images},{"samplers",samplers}};
    uint32_t raw=0,exported=0,terms=0;
    for(uint32_t row=0;row<P::Probes*P::Rays;++row){
        const auto base=P::Header+row*P::Sites*P::SiteWords;
        raw+=words[base]!=0;exported+=words[base+P::ExportSite*P::SiteWords]!=0;
        terms+=words[base+P::TermsSite*P::SiteWords]!=0;
    }
    report["raw_rays_seen"]=raw;report["exports_seen"]=exported;
    report["export_site"]=P::ExportSite;report["terms_site"]=P::TermsSite;
    report["terms_seen"]=terms;report["terms_layout"]="A.rgb,B.rgb,C.rgb,D.rgb; export=(A+B)+(C+D)";
    report["ray_source_history"]=nlohmann::json::array();
    for(size_t i=0;i<probes.size();++i){
        const auto row=rays.is_array()&&i<rays.size()?rays[i]:nlohmann::json{};
        report["ray_source_history"].push_back(DsRaySourceAtConsumer(bindings.buffer_sources[ray_slot].address,probes[i],row));
    }
    report["transition_before_dispatch"]=sel.transition;
    report["site12_atlas_footprints"]=ReadDsRelightAtlasFootprints(context,bindings,words);
    std::ofstream json(folder/(stem+".json"),std::ios::trunc);json<<report;json.close();
    Log::WriteToConsoleAndLog(fmt::format("[DS_RELIGHT_TRACE] frame={} submit={} raw={} exported={} file={} saved={}\n",
        frame,submit,raw,exported,stem,json.good()));
}

static void CaptureDsRelightMaskWriter(RenderContext& context, const PreparedBindings& bindings,
                                       uint64_t submit, DispatchGroups groups,
                                       vk::Buffer indirect = {}, uint64_t offset = 0) {
	const auto& program = *bindings.runtime->program;
	if (program.shader_hash != DemonsRadianceTrace::MaskWriter || !DsTraceEnabled()) return;
	const auto frame = context.GetGpu().GetFrameNum();
	const bool focus=DsTransitionEnabled();
	if ((!focus&&(!DsCoefficientCaptureDue(frame)||ds_relight_mask.frame==frame)) ||
	    Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	for (const auto& buffer: program.info.buffers) {
		const auto slot = buffer.descriptor_index;
		if (!buffer.written || slot >= bindings.buffer_sources.size()) continue;
		const auto& output = bindings.buffer_sources[slot];
		if (output.size != DemonsRadianceTrace::MaskBytes) continue;
		if (indirect && !ReadDsGpuWords(context, indirect, offset, groups)) return;
		if (!groups[0] || !groups[1] || !groups[2]) return;
		auto words = ReadDsBoundWords(context, bindings, slot, output.size / sizeof(uint32_t));
		if (words.empty()) return;
		const auto& resources = *bindings.runtime->resources;
		nlohmann::json dispatch {{"frame", frame}, {"submit", submit}, {"groups", groups},
		    {"indirect", static_cast<bool>(indirect)}, {"output_slot", slot},
		    {"image_descriptors", nlohmann::json::array()}, {"buffers", nlohmann::json::array()},
		    {"srt", resources.flattened_srt}};
		for (const auto& image: resources.images)
			dispatch["image_descriptors"].push_back(image.dwords);
		// The captured acce347 module reads the probe ID first, then the
		// position and bounding record at that ID. It walks 384-byte light
		// records up to the live count in flattened SRT word 22.
		const auto& plan = program.info.buffers;
		if (plan.size() == 5 && resources.buffers.size() == plan.size() &&
		    resources.flattened_srt.size() >= 24 && groups[2] <= DsRadianceExtent &&
		    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[0]).Stride() == 4 &&
		    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[1]).Stride() == 16 &&
		    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[2]).Stride() == 32 &&
		    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[3]).Stride() == 384) {
			const auto ids = ReadDsBoundWords(context, bindings, plan[0].descriptor_index, groups[2]);
			const auto light_count = resources.flattened_srt[22];
			const auto light_capacity =
			    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[3]).NumRecords();
			dispatch["probe_indices"] = ids;
			dispatch["probe_positions"] =
			    ReadDsMaskProbeRecords(context, bindings, plan[1].descriptor_index, ids, 16);
			dispatch["probe_bounds"] =
			    ReadDsMaskProbeRecords(context, bindings, plan[2].descriptor_index, ids, 32);
			dispatch["light_count"] = light_count;
			dispatch["cull_threshold_bits"] = resources.flattened_srt[23];
			dispatch["light_words"] = ReadDsBoundWords(context, bindings, plan[3].descriptor_index,
			    std::min({light_count, light_capacity, focus?64u:384u}) * 96u);
            if(focus&&resources.user_data.size()>=2){
                const auto root=DemonsTransitionTrace::Pointer(resources.user_data[0],resources.user_data[1]);
                dispatch["root_backing"]=DsTransitionBacking(context,root,4);
                const auto& root_info=dispatch["root_backing"];
                if(root_info.contains("words")&&root_info["words"].size()==4){
                    const auto a=root_info["words"].get<std::array<uint32_t,4>>();
                    const auto table=DemonsTransitionTrace::Pointer(a[2],a[3]);
                    dispatch["light_count_backing"]=DsTransitionBacking(context,table,1);
                    if(table&&table<=UINT64_MAX-0x40)dispatch["threshold_backing"]=DsTransitionBacking(context,table+0x3c,1);
                }
                const auto c=DemonsTransitionTrace::Summarize(words,ids);
                dispatch["group1_rows"]=c.group1;dispatch["valid_rows"]=c.valid;
                Log::WriteToConsoleAndLog(fmt::format("[DS_MASK_TRANSITION] frame={} submit={} light_count={} group1_rows={} valid={}\n",frame,submit,light_count,c.group1,c.valid));
            }
		}
		for (size_t i = 0; i < std::min(program.info.buffers.size(), resources.buffers.size()); ++i) {
			const auto& resource = program.info.buffers[i];
			const auto input_slot = resource.descriptor_index;
			if (input_slot >= bindings.buffer_sources.size()) continue;
			const auto& source = bindings.buffer_sources[input_slot];
			nlohmann::json entry {{"slot", input_slot}, {"guest", source.address}, {"bytes", source.size},
			    {"read", resource.read}, {"written", resource.written},
			    {"descriptor", resources.buffers[i].dwords}};
			if (!focus && resource.read && !resource.written)
				entry["prefix_words"] = ReadDsBoundWords(context, bindings, input_slot, 2048);
			dispatch["buffers"].push_back(std::move(entry));
		}
		ds_relight_mask = {frame, submit, output.address, std::move(words), std::move(dispatch)};
        if(focus){ds_transition_masks[ds_transition_mask_next]=ds_relight_mask;ds_transition_mask_next=(ds_transition_mask_next+1)%ds_transition_masks.size();}
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_RELIGHT_MASK_WRITER] frame={} submit={} guest=0x{:x} words={}\n",
		    frame, submit, output.address, ds_relight_mask.words.size()));
		return;
	}
}

// Match the relight shader's indexer: slot 0 maps workgroup Z to the probe
// ID, slot 1 holds 256 packed ray hits per probe, and slots 4/5 supply the
// per-workgroup light masks and the light records they select.
static nlohmann::json CaptureDsRelightInputs(RenderContext& context, const PreparedBindings& bindings,
                                            uint32_t groups_z) {
	const auto& program = *bindings.runtime->program;
	const auto& resources = *bindings.runtime->resources;
	const auto& buffers = program.info.buffers;
	if (buffers.size() < 6 || resources.buffers.size() != buffers.size() ||
	    groups_z > DsRadianceExtent) return {{"status", "unexpected_relight_layout"}};
	const auto map_slot = buffers[0].descriptor_index;
	const auto ray_slot = buffers[1].descriptor_index;
	const auto mask_slot = buffers[4].descriptor_index;
	const auto light_slot = buffers[5].descriptor_index;
	const auto rays_desc = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[1]);
	const auto masks_desc = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[4]);
	const auto lights_desc = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[5]);
	if (rays_desc.Stride() != 8 || rays_desc.NumRecords() != 92160u * 256u ||
	    masks_desc.Stride() != 4 || masks_desc.NumRecords() != 4096u ||
	    lights_desc.Stride() != 384 || ray_slot >= bindings.buffers.size())
		return {{"status", "unexpected_relight_descriptors"}};
	const auto probes = ReadDsBoundWords(context, bindings, map_slot, groups_z);
	const auto masks = ReadDsBoundWords(context, bindings, mask_slot, groups_z * 16u);
	const auto lights = ReadDsBoundWords(context, bindings, light_slot, 384u * 384u / 4u);
	const auto frame = context.GetGpu().GetFrameNum();
	auto slots = nlohmann::json::array();
	for (const auto slot: {map_slot, ray_slot, mask_slot, light_slot}) {
		if (slot >= bindings.buffer_sources.size() || slot >= bindings.buffers.size()) continue;
		const auto& source = bindings.buffer_sources[slot];
		nlohmann::json entry {{"slot", slot}, {"guest", source.address}, {"bytes", source.size},
		                      {"native_range", static_cast<uint64_t>(bindings.buffers[slot].range)}};
		if (const auto* writer = FindDsBufferProducer(frame, source.address, source.size)) {
			entry["bound_writer"] = {{"shader", fmt::format("{:016x}", writer->shader)},
			    {"frame", writer->frame}, {"submit", writer->submit}, {"clear", writer->fill}};
		}
		slots.push_back(std::move(entry));
	}
	nlohmann::json report {{"slots", std::move(slots)}, {"probes", probes}, {"mask_words", masks}};
	report["ray_producer_activity"] = {
	    {"observed", ds_ray_producer_activity.observed},
	    {"valid", ds_ray_producer_activity.valid},
	    {"last_frame", ds_ray_producer_activity.last_frame},
	    {"last_submit", ds_ray_producer_activity.last_submit},
	    {"last_tick", ds_ray_producer_activity.last_tick},
	    {"frames_since_last", ds_ray_producer_activity.last_frame < 0 ? -1 :
	        frame - ds_ray_producer_activity.last_frame}
	};
	if (mask_slot < bindings.buffer_sources.size() && ds_relight_mask.frame == frame &&
	    ds_relight_mask.address == bindings.buffer_sources[mask_slot].address && !masks.empty()) {
		const auto* writer = FindDsBufferProducer(frame, ds_relight_mask.address,
		                                          DemonsRadianceTrace::MaskBytes);
		if (writer && writer->shader == DemonsRadianceTrace::MaskWriter &&
		    writer->submit == ds_relight_mask.submit) {
			const auto count = std::min(masks.size(), ds_relight_mask.words.size());
			const auto begin = ds_relight_mask.words.begin();
			report["mask_writer"] = ds_relight_mask.dispatch;
			report["mask_writer"]["compared_words"] = count;
			report["mask_writer"]["same_words"] = std::equal(begin, begin + count, masks.begin());
			if (!report["mask_writer"].value("same_words", false))
				report["mask_writer"]["output_words"] = ds_relight_mask.words;
		}
	}
	auto light_headers = nlohmann::json::array();
	for (size_t i = 0; i < lights.size() / 96u; ++i)
		light_headers.push_back({lights[i * 96u], lights[i * 96u + 1u]});
	report["light_headers"] = std::move(light_headers);
	auto selected_lights = nlohmann::json::array();
	std::array<uint32_t, 13u * 32u> references {};
	for (size_t z = 0; z < masks.size() / 16u; ++z) {
		const size_t base = z * 16u;
		for (uint32_t group = 0; group < 13u; ++group) {
			if (!(masks[base + 13] & (1u << group))) continue;
			for (uint32_t bits = masks[base + group]; bits; bits &= bits - 1u)
				++references[group * 32u + std::countr_zero(bits)];
		}
	}
	for (size_t index = 0; index < references.size(); ++index) {
		if (references[index] == 0) continue;
		nlohmann::json entry {{"index", index}, {"references", references[index]}};
		if ((index + 1u) * 96u <= lights.size()) {
			const auto begin = lights.begin() + index * 96u;
			entry["words"] = std::vector<uint32_t>(begin, begin + 96u);
		}
		selected_lights.push_back(std::move(entry));
	}
	report["selected_lights"] = std::move(selected_lights);

	const auto offset_word = program.bindings.memory_offset_dword + ray_slot / 4u;
	const auto& native = bindings.buffers[ray_slot];
	if (offset_word >= bindings.shader_data.size() || !native.buffer ||
	    native.range == VK_WHOLE_SIZE || probes.empty()) {
		report["status"] = "ray_view_unavailable";
		return report;
	}
	const auto adjustment = (bindings.shader_data[offset_word] >> (ray_slot % 4u * 8u)) & 255u;
	constexpr size_t words_per_probe = 256u * 2u;
	constexpr size_t bytes_per_probe = words_per_probe * sizeof(uint32_t);
	std::vector<uint32_t> ray_words(probes.size() * words_per_probe);
	std::vector<vk::BufferCopy> copies;
	std::vector<bool> valid(probes.size());
	for (size_t z = 0; z < probes.size(); ++z) {
		const auto begin = uint64_t{probes[z]} * bytes_per_probe + adjustment;
		if (probes[z] >= rays_desc.NumRecords() / 256u ||
		    begin > native.range || bytes_per_probe > native.range - begin) continue;
		copies.emplace_back(native.offset + begin, z * bytes_per_probe, bytes_per_probe);
		valid[z] = true;
	}
	if (!ReadDsGpuWordRanges(context, native.buffer, copies, ray_words)) {
		report["status"] = "ray_readback_unavailable";
		return report;
	}
	// The previous ZIP gave only first_pairs, not the full material/depth
	// values. Preserve every selected 256-ray record, with its exact probe
	// mapping and buffer provenance, so a missing albedo can be traced back
	// to the ray producer rather than blamed on the lighting compositor.
	report["ray_valid_by_z"] = nlohmann::json::array();
	for (const auto is_valid : valid) report["ray_valid_by_z"].push_back(is_valid);
	report["ray_buffer_guest"] = bindings.buffer_sources[ray_slot].address;
	report["ray_buffer_adjustment"] = adjustment;
	const auto& ray_source = bindings.buffer_sources[ray_slot];
	auto& ray_cache = context.GetBufferCache();
	const auto root = Config::GetShaderDumpFolder();
	if (!root.empty() && root.filename() == "ds-lighting-dumps") {
		const auto folder = root / "queue-state";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		if (!ec) {
			const auto name = fmt::format("cs_bcbcc6ba1e68c974_ray_hits_f{}_tick{}.bin",
			    frame, context.GetCommandScheduler().CurrentTick());
			std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
			if (file) {
				file.write(reinterpret_cast<const char*>(ray_words.data()),
				           static_cast<std::streamsize>(ray_words.size() * sizeof(uint32_t)));
				file.close();
				if (file.good()) {
					report["ray_hits_file"] = name;
					report["ray_hits_bytes"] = ray_words.size() * sizeof(uint32_t);
				} else report["ray_hits_file_error"] = "write_failed";
			} else report["ray_hits_file_error"] = "open_failed";
		} else report["ray_hits_file_error"] = "mkdir_failed";
	}
	auto ray_summary = nlohmann::json::array();
	for (size_t z = 0; z < probes.size(); ++z) {
		if (!valid[z]) {
			ray_summary.push_back(nullptr);
			continue;
		}
		const auto words = std::span{ray_words}.subspan(z * words_per_probe, words_per_probe);
		uint64_t digest = 14695981039346656037ull;
		uint32_t nonzero = 0;
		uint32_t albedo = 0;
		uint32_t finite_depth = 0;
		for (size_t i = 0; i < words.size(); i += 2) {
			nonzero += (words[i] | words[i + 1]) != 0;
			albedo += (words[i] & 0x00ffffffu) != 0;
			const auto depth = (words[i + 1] >> 16u) & 0x7fffu;
			finite_depth += depth != 0 && depth < 0x7c00u;
			digest = (digest ^ (uint64_t{words[i + 1]} << 32 | words[i])) * 1099511628211ull;
		}
		const auto address = ray_source.address + uint64_t{probes[z]} * bytes_per_probe;
		nlohmann::json record {{"nonzero_pairs", nonzero}, {"digest", fmt::format("{:016x}", digest)},
		    {"albedo_nonzero", albedo}, {"finite_nonzero_depth", finite_depth},
		    {"first_pairs", std::vector<uint32_t>(words.begin(), words.begin() + 8)}};
		if (ray_source.address != 0) {
			record["guest"] = address;
			record["cpu_modified"] = ray_cache.IsRegionCpuModified(address, bytes_per_probe);
			record["gpu_dirty_bytes"] = ray_cache.HasGpuDirtyBytes(address, bytes_per_probe);
			const auto writer = ray_cache.LastProbeRayWrite(address, bytes_per_probe);
			if (writer.path != nullptr) {
				record["last_watched_page_write"] = {
				    {"path", writer.path}, {"tick", writer.tick}, {"source", writer.source},
				    {"shader", fmt::format("{:016x}", writer.shader)},
				    {"begin", writer.begin}, {"end", writer.end}};
			}
			// Read the raw backing without asking the buffer cache to write
			// the GPU result back first. This distinguishes stale CPU data
			// from the actual bytes read by the relight shader.
			std::array<uint32_t, words_per_probe> backing {};
			if (LibKernel::Memory::TryReadBacking(address, backing.data(), bytes_per_probe)) {
				uint32_t differences = 0;
				uint32_t backing_albedo = 0;
				for (size_t i = 0; i < backing.size(); i += 2) {
					differences += backing[i] != words[i] || backing[i + 1] != words[i + 1];
					backing_albedo += (backing[i] & 0x00ffffffu) != 0;
				}
				record["guest_backing_different_pairs"] = differences;
				record["guest_backing_albedo_nonzero"] = backing_albedo;
			}
			if (const auto* writer = FindDsBufferProducer(frame, address, bytes_per_probe, false)) {
				const auto begin = std::max(address, writer->address);
				const auto end = std::min(address + bytes_per_probe, writer->address + writer->size);
				record["overlapping_write_binding"] = {
				    {"shader", fmt::format("{:016x}", writer->shader)}, {"frame", writer->frame},
				    {"submit", writer->submit}, {"fill", writer->fill},
				    {"offset", begin - address}, {"bytes", end - begin}};
			}
		}
		ray_summary.push_back(std::move(record));
	}
	// Pick two low-color and two fully colored CURRENT consumer probes.
	// Static probe IDs from earlier sessions can refer to a different camera
	// or room and made the frame-206 producer sample inconclusive.
	std::array<uint32_t, 4> selected {};
	size_t selected_count = 0;
	const auto select = [&](int mode, size_t target_count) {
		for (size_t z = 0; z < probes.size() && z < ray_summary.size() &&
		     selected_count < target_count; ++z) {
			if (!valid[z] || !ray_summary[z].is_object()) continue;
			const auto color = ray_summary[z].value("albedo_nonzero", uint32_t{0});
			if (mode == 0 && color > 160) continue;
			if (mode == 1 && color < 240) continue;
			if (std::find(selected.begin(), selected.begin() + selected_count, probes[z]) !=
			    selected.begin() + selected_count) continue;
			selected[selected_count++] = probes[z];
		}
	};
	select(0, 2);
	select(1, 4);
	select(2, 4);
	if (selected_count == selected.size()) {
		ds_ray_producer_targets.probes = selected;
		ds_ray_producer_targets.consumer_frame = frame;
	}
	report["next_ray_producer_targets"] = {
	    {"probes", ds_ray_producer_targets.probes},
	    {"selected_from_consumer_frame", ds_ray_producer_targets.consumer_frame}
	};
	report["ray_hits_by_z"] = std::move(ray_summary);
	report["status"] = "captured";
	return report;
}

static std::vector<uint64_t> ReadDsProbeRadiance(RenderContext& context,
                                                const PreparedBindings& bindings, size_t slot,
                                                nlohmann::json& dispatch) {
	if (slot >= bindings.images.size()) return {};
	const auto& binding = bindings.images[slot];
	if (!binding.image_id || binding.placeholder) return {};
	auto& image = context.GetTextureCache().GetImage(binding.image_id);
	const auto& backing = image.backing;
	const auto view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
	if (view == image.views.end() || view->info.format != vk::Format::eR16G16B16A16Sfloat ||
	    view->info.base_level != 0 || !backing.image || backing.samples != 1 ||
	    backing.image_type != vk::ImageType::e2D ||
	    backing.format != vk::Format::eR16G16B16A16Sfloat ||
	    backing.extent.width != DsRadianceExtent || backing.extent.height != DsRadianceExtent ||
	    !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc)) return {};
	const auto& mapping = view->info.mapping;
	dispatch["radiance_view"] = {{"width", DsRadianceExtent}, {"height", DsRadianceExtent},
	    {"layer", view->info.base_layer}, {"owner", image.info.data.address},
	    {"version", image.ContentVersion()}, {"tick", context.GetCommandScheduler().CurrentTick()},
	    {"swizzle", {static_cast<uint32_t>(mapping.r),
	        static_cast<uint32_t>(mapping.g), static_cast<uint32_t>(mapping.b),
	        static_cast<uint32_t>(mapping.a)}}};
	constexpr size_t texels = size_t{DsRadianceExtent} * DsRadianceExtent;
	constexpr size_t bytes = texels * sizeof(uint64_t);
	auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] = download.Map(bytes, 16u, true);
	if (!mapped) return {};
	download.Commit();
	vk::BufferImageCopy copy {};
	copy.bufferOffset = offset;
	copy.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0,
	                                                   view->info.base_layer, 1};
	copy.imageExtent = vk::Extent3D{DsRadianceExtent, DsRadianceExtent, 1};
	image.Download(std::span{&copy, 1u}, download.Handle(), offset, bytes);
	context.GetCommandScheduler().Finish();
	download.Invalidate(offset, bytes);
	std::vector<uint64_t> result(texels);
	std::memcpy(result.data(), mapped, bytes);
	uint64_t hash = 14695981039346656037ull;
	for (const auto texel: result) hash = (hash ^ texel) * 1099511628211ull;
	dispatch["radiance_digest"] = fmt::format("{:016x}", hash);
	dispatch["radiance_rgb_nonzero"] = std::ranges::count_if(result, [](uint64_t texel) {
		return (texel & 0x00007fff7fff7fffull) != 0;
	});
	return result;
}

void CaptureDsRadianceComputeWriter(RenderContext& context, const PreparedBindings& bindings,
                                   uint64_t submit, DispatchGroups groups,
                                   vk::Buffer indirect = {}, uint64_t offset = 0) {
	if (!DemonsRadianceTrace::Enabled()) return;
	const auto& program = *bindings.runtime->program;
	const auto frame = context.GetGpu().GetFrameNum();
	if (program.shader_hash == DsProbeProjector || !DsCoefficientCaptureDue(frame) ||
	    (ds_radiance_writer.frame == frame && ds_radiance_writer.attempts >= 2) ||
	    Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	for (size_t slot = 0; slot < std::min(bindings.images.size(), program.info.images.size()); ++slot) {
		const auto& binding = bindings.images[slot];
		const auto& range = binding.desc.info.data;
		if (!program.info.images[slot].written || !binding.image_id || binding.placeholder ||
		    binding.desc.type != TextureCache::BindingType::Storage ||
		    !DemonsRadianceTrace::Data(range.address, range.size)) continue;
		if (ds_radiance_writer.frame != frame) ds_radiance_writer = {frame, 0, {}};
		++ds_radiance_writer.attempts;
		const bool valid = !indirect || ReadDsGpuWords(context, indirect, offset, groups);
		if (valid && (!groups[0] || !groups[1] || !groups[2])) return;
		nlohmann::json report {{"shader", fmt::format("{:016x}", program.shader_hash)},
		    {"frame", frame}, {"submit", submit}, {"groups", groups}, {"groups_valid", valid},
		    {"indirect", static_cast<bool>(indirect)}, {"output_slot", slot},
		    {"dma", program.info.uses_dma}, {"bvh", program.info.bvh}};
		if (valid && groups[0] && groups[1] && groups[2]) {
			ReadDsProbeRadiance(context, bindings, slot, report);
			if (program.shader_hash == DsRadianceRelight)
				report["relight_inputs"] = CaptureDsRelightInputs(context, bindings, groups[2]);
		}
		const auto& resources = *bindings.runtime->resources;
		report["image_descriptors"] = nlohmann::json::array();
		for (const auto& image: resources.images) report["image_descriptors"].push_back(image.dwords);
		report["image_bindings"] = nlohmann::json::array();
		for (const auto& image: bindings.images) {
			nlohmann::json entry {{"guest", image.desc.info.data.address},
			    {"placeholder", image.placeholder}, {"bound", static_cast<bool>(image.image_id)}};
			if (image.image_id && !image.placeholder) {
				const auto& owner = context.GetTextureCache().GetImage(image.image_id);
				entry["owner"] = owner.info.data.address;
				entry["version"] = owner.ContentVersion();
			}
			report["image_bindings"].push_back(std::move(entry));
		}
		report["buffer_descriptors"] = nlohmann::json::array();
		for (const auto& resource: resources.buffers)
			report["buffer_descriptors"].push_back(resource.dwords);
		// Preserve the exact relight inputs at the queue-comparison frame.
		report["srt"] = resources.flattened_srt;
		report["shader_data"] = bindings.shader_data;
		report["buffer_inputs"] = nlohmann::json::array();
		for (const auto& resource : program.info.buffers) {
			const auto index = resource.descriptor_index;
			if (index >= bindings.buffer_sources.size()) continue;
			const auto& source = bindings.buffer_sources[index];
			nlohmann::json entry {{"slot", index}, {"guest", source.address},
			    {"bytes", source.size}, {"read", resource.read}, {"written", resource.written}};
			if (resource.read && source.address && source.size)
				entry["prefix_words"] = ReadDsBoundWords(context, bindings, index, 256);
			report["buffer_inputs"].push_back(std::move(entry));
		}
		report["checkpoint"] = DsCoefficientCaptureDue(frame);
		const auto digest = report.value("radiance_digest", std::string{"unavailable"});
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_RADIANCE_COMPUTE_OUTPUT] frame={} submit={} shader=0x{:016x} "
		    "groups_valid={} x={} y={} z={} digest={}\n",
		    frame, submit, program.shader_hash, valid, groups[0], groups[1], groups[2], digest));
		if (ds_radiance_writer.dispatch.empty() || report.contains("radiance_digest"))
			ds_radiance_writer.dispatch = std::move(report);
		return;
	}
}

void CaptureDsProbeSourceWrite(RenderContext& context, const PreparedBindings& bindings,
                               uint64_t submit, DispatchGroups groups,
                               vk::Buffer indirect_buffer = {}, uint64_t indirect_offset = 0) {
	const auto& program = *bindings.runtime->program;
	if (program.shader_hash == DsCoefficientWriter || program.shader_hash == 0xc03afccb96a13c5dull ||
	    !DsTraceEnabled()) return;
	const auto frame = context.GetGpu().GetFrameNum();
	if (!DsCoefficientCaptureDue(frame) ||
	    Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	static int32_t last_frame = -1;
	static uint32_t count = 0;
	if (frame != last_frame) {
		last_frame = frame;
		count = 0;
	}
	if (count >= ds_probe_source_snapshots.size()) return;
	const auto& resources = *bindings.runtime->resources;
	for (const auto& resource : program.info.buffers) {
		const auto slot = resource.descriptor_index;
		if (!resource.written || slot >= bindings.buffer_sources.size()) continue;
		const auto& output = bindings.buffer_sources[slot];
		if (!output.address || output.size != DsCoefficientBytes) continue;
		const auto* writer = FindDsBufferProducer(frame, output.address, output.size);
		if (!writer || writer->frame != frame || writer->shader != program.shader_hash ||
		    writer->submit != submit) continue;
		if (indirect_buffer && !ReadDsGpuWords(context, indirect_buffer, indirect_offset, groups)) return;
		if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0) return;
		auto words = ReadDsBoundWords(context, bindings, slot, DsCoefficientBytes / 4);
		if (words.empty()) return;
		auto descriptors = nlohmann::json::array();
		for (size_t i = 0; i < std::min(resources.buffers.size(), program.info.buffers.size()); ++i) {
			const auto& input = program.info.buffers[i];
			const auto input_slot = input.descriptor_index;
			if (input_slot >= bindings.buffer_sources.size()) continue;
			const auto& source = bindings.buffer_sources[input_slot];
			descriptors.push_back({{"slot", input_slot}, {"guest", source.address},
			    {"bytes", source.size}, {"read", input.read}, {"written", input.written},
			    {"descriptor", resources.buffers[i].dwords}});
		}
		auto images = nlohmann::json::array();
		for (size_t i = 0; i < std::min(resources.images.size(), bindings.images.size()); ++i) {
			const auto& binding = bindings.images[i];
			images.push_back({{"slot", i}, {"guest", binding.desc.info.data.address},
			    {"placeholder", binding.placeholder}, {"descriptor", resources.images[i].dwords}});
		}
		nlohmann::json dispatch {{"groups", groups}, {"output_slot", slot}, {"bvh", program.info.bvh},
		    {"buffers", std::move(descriptors)}, {"images", std::move(images)},
		    {"srt", resources.flattened_srt}, {"shader_data", bindings.shader_data}};
		std::vector<uint64_t> radiance;
		if (program.shader_hash == DsProbeProjector && slot == 1) {
			// This compute shader only reads the radiance image and probe indices.
			// Copy them here with its output, before another dispatch can replace them.
			dispatch["probe_indices"] = ReadDsBoundWords(context, bindings, 0, DsRadianceExtent);
			radiance = ReadDsProbeRadiance(context, bindings, 0, dispatch);
			dispatch["radiance_bytes"] = radiance.size() * sizeof(uint64_t);
			if (ds_radiance_writer.frame == frame)
				dispatch["sampled_compute_writer"] = ds_radiance_writer.dispatch;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_PROBE_RADIANCE] frame={} submit={} bytes={} indices={}\n",
			    frame, submit, radiance.size() * sizeof(uint64_t), dispatch["probe_indices"].size()));
		}
		ds_probe_source_snapshots[ds_next_probe_snapshot] = std::make_shared<DsProbeSourceWrite>(
		    DsProbeSourceWrite{output.address, writer->serial, std::move(words),
		                       std::move(dispatch), std::move(radiance)});
		ds_next_probe_snapshot = (ds_next_probe_snapshot + 1) % ds_probe_source_snapshots.size();
		if (++count >= ds_probe_source_snapshots.size()) break;
		indirect_buffer = vk::Buffer{};
	}
}

std::optional<DsCoefficientDispatch> BeginDsCoefficientDispatch(
    RenderContext& context, const PreparedBindings& bindings, uint64_t submit) {
	const auto& program = *bindings.runtime->program;
	if (program.shader_hash != DsCoefficientWriter || !DsTraceEnabled()) return {};
	const auto frame = context.GetGpu().GetFrameNum();
	if (!DsCoefficientCaptureDue(frame) ||
	    Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return {};
	const auto& resources = *bindings.runtime->resources;
	for (const auto& resource : program.info.buffers) {
		const auto slot = resource.descriptor_index;
		if (!resource.written || slot >= bindings.buffer_sources.size()) continue;
		const auto& output = bindings.buffer_sources[slot];
		if (output.address == 0 || output.size != DsCoefficientBytes) continue;
		static int32_t sampled_frame = -1;
		static uint32_t captures = 0;
		if (sampled_frame != frame) {
			sampled_frame = frame;
			captures = 0;
			ds_coefficient_dispatch.reset();
		}
		if (captures >= 4) return {};
		++captures;
		DsCoefficientDispatch trace {frame, submit, output.address, slot, ds_next_buffer_producer,
		                            0, {}, {}, {}, {}, {}, {}};
		trace.before = ReadDsBoundWords(context, bindings, slot, DsCoefficientBytes / 4);
		// In the exported seven-slot shader, slot 4 receives the updated
		// coefficients. Slots 2 and 3 supply its weight and 64-byte input
		// coefficients, both indexed by the same probe ID loaded from slot 1.
		if (slot == 4 && bindings.buffers.size() == 7 && program.bindings.UsesPushData()) {
			trace.source_weights = ReadDsBoundWords(context, bindings, 2, DsCoefficientBytes / 64);
			trace.source_coefficients = ReadDsBoundWords(context, bindings, 3, DsCoefficientBytes / 4);
		}
		auto buffers = nlohmann::json::array();
		for (size_t i = 0; i < program.info.buffers.size(); ++i) {
			const auto& binding = program.info.buffers[i];
			const auto input_slot = binding.descriptor_index;
			if (input_slot >= bindings.buffer_sources.size()) continue;
			const auto& source = bindings.buffer_sources[input_slot];
			buffers.push_back({
			    {"slot", input_slot}, {"guest", source.address}, {"bytes", source.size},
			    {"read", binding.read}, {"written", binding.written},
			    {"descriptor", resources.buffers[i].dwords},
			    {"prefix_before", input_slot != slot && binding.read
			        ? ReadDsBoundWords(context, bindings, input_slot, 4096) : std::vector<uint32_t> {}},
			});
		}
		auto images = nlohmann::json::array();
		for (size_t i = 0; i < std::min(bindings.images.size(), resources.images.size()); ++i) {
			const auto& image = bindings.images[i];
			images.push_back({{"slot", i}, {"guest", image.desc.info.data.address},
			                  {"placeholder", image.placeholder}, {"descriptor", resources.images[i].dwords}});
		}
		trace.inputs = {
		    {"buffers", std::move(buffers)}, {"images", std::move(images)},
		    {"srt", resources.flattened_srt}, {"user_data", resources.user_data},
		    {"shader_data", bindings.shader_data}, {"bvh", program.info.bvh},
		};
		if (slot == 4 && bindings.buffer_sources.size() > 3) {
			const auto& source = bindings.buffer_sources[3];
			const auto* writer = FindDsProbeSourceWriter(frame, source.address, source.size);
			if (writer) {
				trace.inputs["source_writer"] = {
				    {"shader", fmt::format("{:016x}", writer->shader)},
				    {"frame", writer->frame}, {"submit", writer->submit},
				    {"guest", writer->address}, {"bytes", writer->size}, {"fill", writer->fill},
				};
				for (const auto& snapshot : ds_probe_source_snapshots) {
					if (snapshot && snapshot->address == source.address &&
					    snapshot->serial == writer->serial) {
						trace.source_write = snapshot;
						break;
					}
				}
			}
			LogDsBufferProducer(frame, DsCoefficientWriter, 0, "coefficient_source", 3,
			                    source.address, source.size);
		}
		return trace;
	}
	return {};
}

void EndDsCoefficientDispatch(RenderContext& context, const PreparedBindings& bindings,
                              std::optional<DsCoefficientDispatch> trace, DispatchGroups groups,
                              vk::Buffer indirect_buffer = {}, uint64_t indirect_offset = 0) {
	if (!trace) return;
	trace->writer_sequence = ds_next_buffer_producer;
	const bool valid_groups = !indirect_buffer ||
	    ReadDsGpuWords(context, indirect_buffer, indirect_offset, groups);
	trace->inputs["workgroups"] = groups;
	trace->inputs["workgroups_valid"] = valid_groups;
	if (valid_groups && groups[0] && groups[1] && groups[2]) {
		trace->after = ReadDsBoundWords(context, bindings, trace->slot, DsCoefficientBytes / 4);
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_COEFFICIENT_DISPATCH] frame={} submit={} address=0x{:x} x={} y={} z={} "
	    "groups_valid={} before_bytes={} after_bytes={}\n",
	    trace->frame, trace->submit, trace->address, groups[0], groups[1], groups[2],
	    valid_groups, trace->before.size() * 4, trace->after.size() * 4));
	auto snapshot = std::make_shared<const DsCoefficientDispatch>(std::move(*trace));
	ds_coefficient_dispatch = snapshot;
	ds_coefficient_snapshots[ds_next_coefficient_snapshot++ % ds_coefficient_snapshots.size()] =
	    std::move(snapshot);
}

nlohmann::json CompareDsCoefficientDispatch(int32_t frame, uint64_t address, uint64_t bytes,
                                            std::span<const uint32_t> indices,
                                            std::span<const uint32_t> index_valid,
                                            std::span<const uint32_t> valid_words,
                                            std::span<const uint32_t> observed,
                                            std::span<const uint64_t>& radiance) {
	const auto* writer = FindDsBufferProducer(frame, address, bytes);
	if (!writer) return {{"status", "no_bound_writer"}};
	nlohmann::json result {{"writer_shader", fmt::format("{:016x}", writer->shader)},
	                      {"writer_frame", writer->frame}, {"writer_submit", writer->submit}};
	const DsCoefficientDispatch* capture = nullptr;
	for (const auto& candidate : ds_coefficient_snapshots) {
		if (candidate && candidate->frame == writer->frame &&
		    candidate->submit == writer->submit && candidate->address == address &&
		    candidate->after.size() * 4 == bytes) {
			capture = candidate.get();
			break;
		}
	}
	if (!capture || writer->shader != DsCoefficientWriter) {
		result["status"] = "writer_snapshot_unavailable";
		return result;
	}
	const auto slot = static_cast<size_t>(writer - ds_buffer_producers.data());
	const auto age = ds_next_buffer_producer - capture->writer_sequence;
	const auto offset = (capture->writer_sequence + ds_buffer_producers.size() - 1 -
	                     slot) % ds_buffer_producers.size();
	if (age >= ds_buffer_producers.size() || offset >= ds_buffer_producers.size() - age ||
	    offset >= capture->writer_sequence - capture->writer_begin) {
		result["status"] = "writer_snapshot_replaced";
		return result;
	}
	result["status"] = "captured";
	result["inputs_before"] = capture->inputs;
	auto before = nlohmann::json::array();
	auto after = nlohmann::json::array();
	auto source = nlohmann::json::array();
	auto source_after_writer = nlohmann::json::array();
	auto weights = nlohmann::json::array();
	size_t compared = 0;
	size_t different = 0;
	size_t changed = 0;
	size_t source_records = 0;
	size_t source_different = 0;
	constexpr size_t payload_words = 14;
	for (size_t i = 0; i < indices.size(); ++i) {
		const size_t probe = indices[i] & 0x00ffffffu;
		const size_t begin = probe * 16u;
		if (!index_valid[i] || valid_words[i] != payload_words ||
		    begin + payload_words > capture->after.size()) {
			before.push_back(nullptr);
			after.push_back(nullptr);
			source.push_back(nullptr);
			source_after_writer.push_back(nullptr);
			weights.push_back(nullptr);
			continue;
		}
		const auto words = std::span{capture->after}.subspan(begin, payload_words);
		after.push_back(std::vector<uint32_t>(words.begin(), words.end()));
		if (begin + 16 <= capture->source_coefficients.size()) {
			const auto input = std::span{capture->source_coefficients}.subspan(begin, 16);
			source.push_back(std::vector<uint32_t>(input.begin(), input.end()));
			if (capture->source_write && begin + 16 <= capture->source_write->words.size()) {
				const auto produced = std::span{capture->source_write->words}.subspan(begin, 16);
				source_after_writer.push_back(std::vector<uint32_t>(produced.begin(), produced.end()));
				++source_records;
				source_different += !std::ranges::equal(produced, input);
			} else {
				source_after_writer.push_back(nullptr);
			}
		} else {
			source.push_back(nullptr);
			source_after_writer.push_back(nullptr);
		}
		if (probe < capture->source_weights.size()) weights.push_back(capture->source_weights[probe]);
		else weights.push_back(nullptr);
		++compared;
		different += !std::ranges::equal(words, observed.subspan(i * payload_words, payload_words));
		if (begin + payload_words <= capture->before.size()) {
			const auto prior = std::span{capture->before}.subspan(begin, payload_words);
			before.push_back(std::vector<uint32_t>(prior.begin(), prior.end()));
			changed += !std::ranges::equal(prior, words);
		} else {
			before.push_back(nullptr);
		}
	}
	result["compared_records"] = compared;
	result["changed_by_writer"] = capture->before.empty() ? nlohmann::json() : nlohmann::json(changed);
	result["different_at_atlas"] = different;
	result["before_words"] = std::move(before);
	result["after_words"] = std::move(after);
	result["matched_source_coefficients"] = std::move(source);
	if (capture->source_write) {
		result["source_dispatch"] = capture->source_write->dispatch;
		radiance = capture->source_write->radiance;
		result["source_records_compared"] = source_records;
		result["source_records_different_at_update"] = source_different;
		result["source_words_after_writer"] = std::move(source_after_writer);
	}
	result["matched_source_weights"] = std::move(weights);
	return result;
}

} // namespace

static nlohmann::json ReadDsAtlasWriterTexels(RenderContext& context,
                                            const PreparedBindings& bindings,
                                            std::span<const uint32_t> srt,
                                            std::span<const uint32_t> groups_z,
                                            std::span<const uint32_t> index_values) {
	auto& textures = context.GetTextureCache();
	for (const auto& binding: bindings.images) {
		if (!binding.image_id || binding.placeholder ||
		    binding.desc.type != TextureCache::BindingType::Storage) continue;
		auto& image = textures.GetImage(binding.image_id);
		const auto& host = image.backing;
		if (host.format != vk::Format::eR16G16B16A16Sfloat || host.samples != 1 ||
		    !(host.usage & vk::ImageUsageFlagBits::eTransferSrc)) continue;
		const auto found = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
		if (found == image.views.end()) continue;
		const auto view = found->info;
		if (view.base_level >= host.mip_levels || view.base_layer >= host.layers ||
		    (view.type != vk::ImageViewType::e2D && view.type != vk::ImageViewType::e2DArray)) continue;
		const auto width = std::max(host.extent.width >> view.base_level, 1u);
		const auto height = std::max(host.extent.height >> view.base_level, 1u);
		struct Sample { uint32_t z, x, y, layer; };
		constexpr size_t max_samples = 16;
		std::array<Sample, max_samples> samples {};
		uint32_t count = 0;
		const auto sample_count = std::min(samples.size(), groups_z.size());
		for (size_t i = 0; i < sample_count; ++i) {
			const auto row = i * groups_z.size() / sample_count;
			const auto index = index_values[row] & 0x00ffffffu;
			const auto x = ((index & srt[24]) << 4u) + 2u;
			const auto y = (((index >> (srt[22] & 31u)) & srt[25]) << 4u) + 2u;
			const auto layer = view.type == vk::ImageViewType::e2DArray
			    ? (index >> (srt[23] & 31u)) & srt[26] : 0u;
			if (x >= width || y >= height ||
			    layer >= std::min(view.layer_count, host.layers - view.base_layer)) continue;
			samples[count++] = {groups_z[row], x, y, view.base_layer + layer};
		}
		if (count == 0) continue;
		constexpr uint32_t texel_bytes = 8;
		const uint32_t bytes = count * texel_bytes;
		auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
		auto [mapped, offset] = download.Map(bytes, 16u, true);
		if (!mapped) return {{"error", "staging_unavailable"}};
		download.Commit();
		std::array<vk::BufferImageCopy, max_samples> copies {};
		for (uint32_t i = 0; i < count; ++i) {
			const auto& sample = samples[i];
			auto& copy = copies[i];
			copy.bufferOffset = offset + i * texel_bytes;
			copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, view.base_level, sample.layer, 1};
			copy.imageOffset = vk::Offset3D {static_cast<int32_t>(sample.x), static_cast<int32_t>(sample.y), 0};
			copy.imageExtent = vk::Extent3D {1, 1, 1};
		}
		image.Download(std::span{copies}.first(count), download.Handle(), offset, bytes);
		context.GetCommandScheduler().Finish();
		download.Invalidate(offset, bytes);
		auto texels = nlohmann::json::array();
		for (uint32_t i = 0; i < count; ++i) {
			const auto& sample = samples[i];
			std::array<uint16_t, 4> rgba {};
			std::memcpy(rgba.data(), mapped + i * texel_bytes, texel_bytes);
			texels.push_back({{"z", sample.z}, {"x", sample.x}, {"y", sample.y},
			                  {"layer", sample.layer}, {"rgba16_bits", rgba}});
		}
		return {{"owner", image.info.data.address}, {"version", image.ContentVersion()},
		        {"mip", view.base_level}, {"texels", std::move(texels)}};
	}
	return {{"error", "no_readable_storage_view"}};
}

static void ProbeDsAtlasInputs(RenderContext& context, const PreparedBindings& bindings,
                              uint64_t submit, DispatchGroups groups,
                              vk::Buffer indirect_buffer = {}, uint64_t indirect_offset = 0) {
	const auto& program = *bindings.runtime->program;
	if (program.shader_hash != 0xc03afccb96a13c5dull || !DsTraceEnabled()) return;
	const auto frame = context.GetGpu().GetFrameNum();
	static DemonsRollingProbe::FrameGate gate;
	// A periodic atlas-only sample is still useful. Once a real writer was
	// captured, however, keep trying live consumers in that frame until one
	// uses its output. Empty dispatches cannot consume the paired sample.
	const bool paired_candidate = DsCoefficientCaptureDue(frame) && ds_coefficient_dispatch &&
	    !ds_coefficient_dispatch->after.empty() && ds_coefficient_dispatch->frame == frame;
	const bool periodic = gate.Due(frame, 240);
	static const bool track_tiles=[] {const char* v=std::getenv("KYTY_DS_COLOR_PROBE_TARGETED");return v&&v[0]=='1'&&v[1]=='\0';}();
	if (!periodic && !paired_candidate && !track_tiles) return;
	const auto report = [frame, submit](const char* reason) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_LIGHT_ATLAS_INPUT] frame={} submit={} capture={}\n", frame, submit, reason));
	};
	if (indirect_buffer && !ReadDsGpuWords(context, indirect_buffer, indirect_offset, groups)) {
		if(track_tiles)ds_atlas_tile_history.Invalidate();
		report("indirect_args_unreadable");
		return;
	}
	if(track_tiles)RememberDsAtlasTileInputs(context,bindings,submit,groups);
	if (!periodic && !paired_candidate) return;
	if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0) {
		if (periodic) report("empty_dispatch");
		return;
	}
	const auto& srt = bindings.runtime->resources->flattened_srt;
	const auto packed_offset = program.bindings.memory_offset_dword;
	if (srt.size() < 28 || bindings.buffers.size() < 2 || bindings.buffer_sources.size() < 2 ||
	    packed_offset >= bindings.shader_data.size() ||
	    !program.bindings.UsesPushData() || program.info.group_pack > 1) {
		report("unexpected_shader_layout");
		return;
	}
	const auto& indices = bindings.buffers[0];
	const auto& coefficients = bindings.buffers[1];
	const uint32_t index_adjustment = bindings.shader_data[packed_offset] & 255u;
	const uint32_t coefficient_adjustment = (bindings.shader_data[packed_offset] >> 8u) & 255u;
	if (!indices.buffer || !coefficients.buffer ||
	    indices.range == VK_WHOLE_SIZE || coefficients.range == VK_WHOLE_SIZE) {
		report("unexpected_buffer_binding");
		return;
	}

	// The four captured variants use SRT word 27 plus WorkGroupID.z to select
	// a dword from slot 0. Its low 24 bits index a 64-byte coefficient record
	// in slot 1. Only the first 14 dwords of that record are read by the shader.
	constexpr uint32_t coefficient_words = 14;
	constexpr uint32_t max_records = 4096;
	const uint32_t count = std::min(groups[2], max_records);
	std::vector<uint32_t> index_values(count);
	std::vector<uint32_t> coefficient_values(size_t {count} * coefficient_words);
	std::vector<vk::BufferCopy> index_copies;
	std::vector<vk::BufferCopy> coefficient_copies;
	std::vector<uint32_t> selected_groups;
	std::vector<uint32_t> index_valid(count);
	std::vector<uint32_t> coefficient_valid_words(count);
	selected_groups.reserve(count);
	index_copies.reserve(count);
	coefficient_copies.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		const auto z = static_cast<uint32_t>(uint64_t {i} * groups[2] / count);
		selected_groups.push_back(z);
		const uint32_t offset = (srt[27] + z) * 4u + index_adjustment;
		if (offset < index_adjustment || offset > UINT32_MAX - 3u ||
		    (offset + 3u) / 4u >= indices.range / 4u) continue;
		index_valid[i] = 1;
		index_copies.emplace_back(indices.offset + (offset & ~3u), uint64_t {i} * 4u, 4u);
	}
	if (!index_copies.empty() &&
	    !ReadDsGpuWordRanges(context, indices.buffer, index_copies, index_values)) {
		report("indices_unreadable");
		return;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const uint64_t offset = uint64_t {index_values[i] & 0x00ffffffu} * 64u +
		                        (coefficient_adjustment & ~3u);
		const uint64_t available = offset < coefficients.range ? coefficients.range - offset : 0;
		const auto valid = std::min<uint64_t>(coefficient_words, available / 4u);
		coefficient_valid_words[i] = static_cast<uint32_t>(valid);
		const auto size = valid * 4u;
		if (size != 0)
			coefficient_copies.emplace_back(coefficients.offset + offset,
			                                uint64_t {i} * coefficient_words * 4u, size);
	}
	if (!coefficient_copies.empty() &&
	    !ReadDsGpuWordRanges(context, coefficients.buffer, coefficient_copies, coefficient_values)) {
		report("coefficients_unreadable");
		return;
	}
	std::span<const uint64_t> radiance;
	auto writer = coefficient_adjustment == 0 ? CompareDsCoefficientDispatch(
	    frame, bindings.buffer_sources[1].address, bindings.buffer_sources[1].size,
	    index_values, index_valid, coefficient_valid_words, coefficient_values, radiance)
	    : nlohmann::json{{"status", "unaligned_atlas_buffer"}};
	const bool paired = writer.value("compared_records", size_t{0}) > 0;
	if (!periodic && !paired) return;
	LogDsBufferProducer(frame, program.shader_hash, 0, "atlas_index", 0,
	                    bindings.buffer_sources[0].address, bindings.buffer_sources[0].size);
	LogDsBufferProducer(frame, program.shader_hash, 0, "atlas_coefficients", 1,
	                    bindings.buffer_sources[1].address, bindings.buffer_sources[1].size);
	const auto root = Config::GetShaderDumpFolder();
	if (root.filename() != "ds-lighting-dumps") {
		report("dump_directory_unavailable");
		return;
	}
	const auto folder = root / "queue-state";
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	// Protect low-radiance frames from both periodic atlas-only samples and
	// subsequent pairs with brighter output. 0x1419 is binary16 for 0.001.
	const bool low_radiance = !radiance.empty() && std::ranges::none_of(radiance, [](uint64_t texel) {
		for (uint32_t shift: {0u, 16u, 32u}) {
			const auto component = (texel >> shift) & 0x7fffu;
			if (component >= 0x1419u && component < 0x7c00u) return true;
		}
		return false;
	});
	static std::atomic<uint32_t> next_pair {0}, next_periodic {0}, next_dark {0};
	// Rolling slots overwrote the recorded event (355) and bright reference.
	// Keep actual paired writers by unique guest frame + submit, bounded
	// to 16 records. Nonpaired scout slots continue rotating as before.
	static uint32_t saved_pairs = 0;
	static bool bright_saved = false;
	const bool late_pair = paired && DemonsAdaptiveCapture::Current().PostTransitionDarkDue(frame);
	const bool event_pair = paired && DemonsAdaptiveCapture::Current().CaptureDue(frame);
	const bool bright_pair = paired && !bright_saved &&
	    DemonsAdaptiveCapture::Current().BrightSceneActive(frame);
	const bool preserve_pair = (event_pair || bright_pair) && saved_pairs < 16u;
	const auto file_slot = !paired ? 4u + next_periodic.fetch_add(1, std::memory_order_relaxed) % 2u
	                       : low_radiance ? 6u + next_dark.fetch_add(1, std::memory_order_relaxed) % 2u
	                                      : next_pair.fetch_add(1, std::memory_order_relaxed) % 4u;
	const auto suffix = preserve_pair
	    ? fmt::format("{}_f{}_s{}_n{}", late_pair ? "post_transition" :
	                  event_pair ? "event" : "bright", frame, submit, saved_pairs)
	    : fmt::format("{}", file_slot);
	const auto name = fmt::format("cs_c03afccb96a13c5d_atlas_inputs_{}.json", suffix);
	std::ofstream file(folder / name, std::ios::trunc);
	if (file) {
		if (!radiance.empty()) {
			const auto radiance_name = fmt::format("cs_567d21ad45386c8d_radiance_{}.bin", suffix);
			std::ofstream image_file(folder / radiance_name, std::ios::binary | std::ios::trunc);
			image_file.write(reinterpret_cast<const char*>(radiance.data()),
			                 static_cast<std::streamsize>(radiance.size_bytes()));
			image_file.close();
			if (image_file.good()) writer["source_dispatch"]["radiance_file"] = radiance_name;
		}
		const nlohmann::json payload {
		    {"frame", frame}, {"submit", submit}, {"workgroups", groups},
		    {"srt_22_to_27", std::vector<uint32_t>(srt.begin() + 22, srt.begin() + 28)},
		    {"index_guest", bindings.buffer_sources[0].address}, {"index_range", indices.range},
		    {"index_adjustment", index_adjustment},
		    {"coefficient_guest", bindings.buffer_sources[1].address},
		    {"coefficient_range", coefficients.range},
		    {"coefficient_adjustment", coefficient_adjustment},
		    {"selected_z", selected_groups}, {"index_words", index_values},
		    {"index_in_bounds", index_valid},
		    {"coefficient_words_per_record", coefficient_words},
		    {"coefficient_in_bounds_words", coefficient_valid_words},
		    {"coefficient_words", coefficient_values},
		    {"coefficient_writer", writer},
		    {"atlas_after_writer", ReadDsAtlasWriterTexels(context, bindings, srt,
		                                                  selected_groups, index_values)},
		};
		file << payload;
		file.close();
	}
	if (file.good() && preserve_pair) {
		++saved_pairs;
		if (bright_pair) bright_saved = true;
	}
	if (file.good() && paired) {
		DemonsAdaptiveCapture::Current().NoteCaptured(frame);
		// Supplemental dark-frame captures must not reset the periodic cadence.
		if (DsCoefficientPairDue(frame)) ds_last_coefficient_pair = frame;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_COEFFICIENT_PAIRED] frame={} writer_submit={} atlas_submit={} records={}\n",
		    frame, writer.value("writer_submit", uint64_t{0}), submit,
		    writer.value("compared_records", size_t{0})));
	}
	report(file.good() ? name.c_str() : "file_write_failed");
}

static std::vector<uint32_t> ReadDsBoundWords(RenderContext& context,
                                               const PreparedBindings& bindings,
                                               uint32_t slot, size_t max_words) {
	if (slot >= bindings.buffers.size() || slot >= bindings.buffer_sources.size()) return {};
	const auto& native = bindings.buffers[slot];
	const auto& source = bindings.buffer_sources[slot];
	const auto& program = *bindings.runtime->program;
	const auto offset_word = program.bindings.memory_offset_dword + slot / 4u;
	if (offset_word >= bindings.shader_data.size() || !native.buffer ||
	    native.range == VK_WHOLE_SIZE) return {};
	const auto adjustment = (bindings.shader_data[offset_word] >> ((slot % 4u) * 8u)) & 255u;
	if (adjustment >= native.range) return {};
	const auto count = std::min<uint64_t>(max_words,
	    std::min<uint64_t>(source.size, native.range - adjustment) / sizeof(uint32_t));
	std::vector<uint32_t> words(count);
	if (!ReadDsGpuWords(context, native.buffer, native.offset + adjustment, words)) words.clear();
	return words;
}

// This acquisition follows the actual indexed-indirect stream learned by the
// CP. A writable descriptor is a candidate, never a certificate of a store.
// Keep its consumed scalar inputs and native buffer operands before dispatch;
// only a retained producer generation owns a persistent operand blob.
struct DsIndirectProducerCapture {
    struct Stream {
        DemonsIndirectWitness::Target target;
        vk::Buffer native {};
        uint64_t offset = 0;
        nlohmann::json before;
        uint64_t before_seq = 0;
    };
    std::vector<Stream> streams;
    nlohmann::json inputs;
    std::vector<uint8_t> operand_bytes;
};

static nlohmann::json DsIndirectProducerTarget(RenderContext& context,
    DemonsIndirectWitness::Target target, vk::Buffer native, uint64_t offset) {
    auto& cache = context.GetBufferCache();
    nlohmann::json out = {{"address", target.address}, {"bytes", target.bytes},
        {"stream_id", target.stream_id}, {"epoch_before", DemonsIndirectWitness::Epoch(target.stream_id)},
        {"tick_before", context.GetCommandScheduler().CurrentTick()},
        {"mapping_epoch_before", cache.UnregisterEpoch()},
        {"gpu_dirty_generation_before", cache.GpuDirtyGeneration()},
        {"gpu_dirty", cache.HasGpuDirtyBytes(target.address, target.bytes)},
        {"pending_publication", cache.HasPendingBackingPublication(target.address, target.bytes)},
        {"cpu_modified", cache.IsRegionCpuModified(target.address, target.bytes)},
        {"gpu_modified", cache.IsRegionGpuModified(target.address, target.bytes)},
        {"capture_status", "native_buffer_missing"}, {"readback_wait", false}};
    // No ObtainBuffer/ReadMemory here: a diagnostic must not manufacture a
    // publication while trying to observe the producer's existing allocation.
    if (target.bytes == DemonsIndirectWitness::BatchBytes) {
        std::array<uint32_t, 15> backing {};
        const bool backing_ok = LibKernel::Memory::TryReadBacking(target.address, backing.data(), target.bytes);
        out["backing_read_ok"] = backing_ok;
        if (backing_ok) out["backing_words"] = backing;
        out["native_source"] = native ? "bound_writer_descriptor" : "existing_cache_allocation";
        if (!native) if (auto* source = cache.PeekVertexProducerBuffer(target.address, target.bytes)) {
            native = source->Handle();
            offset = source->Offset(target.address);
        }
        if (native) {
            out["native_buffer"] = std::hash<VkBuffer>{}(static_cast<VkBuffer>(native));
            out["native_offset"] = offset;
            std::array<uint32_t, 15> words {};
            const bool ok = !(offset & 3u) && ReadDsGpuWords(context, native, offset, words, true);
            out["readback_wait"] = !(offset & 3u);
            out["capture_status"] = ok ? "complete" : "native_read_failed_or_unaligned";
            if (ok) out["native_words"] = words;
        }
    } else {
        out["capture_status"] = "unexpected_target_size";
    }
    out["epoch_after"] = DemonsIndirectWitness::Epoch(target.stream_id);
    out["mapping_epoch_after"] = cache.UnregisterEpoch();
    out["gpu_dirty_generation_after"] = cache.GpuDirtyGeneration();
    out["tick_after"] = context.GetCommandScheduler().CurrentTick();
    return out;
}

struct DsIndirectOperandSlice {
    vk::Buffer buffer {};
    uint64_t offset = 0, copy_bytes = 0, skip = 0, bytes = 0;
    size_t binding = 0;
};

static bool ReadDsIndirectOperandSlices(RenderContext& context,
    std::span<const DsIndirectOperandSlice> slices, std::vector<uint8_t>& result) {
    if (slices.empty()) return true;
    uint64_t total = 0, payload = 0;
    for (const auto& slice : slices) {
        if (!slice.buffer || !slice.copy_bytes || (slice.offset & 3u) || (slice.copy_bytes & 3u) ||
            slice.skip + slice.bytes > slice.copy_bytes ||
            slice.copy_bytes > DemonsIndirectWitness::GenerationBudget + 4096u - total) return false;
        total += slice.copy_bytes;
        payload += slice.bytes;
    }
    auto& scheduler = context.GetCommandScheduler();
    auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped, destination] = download.Map(total, 16u, true);
    if (!mapped) return false;
    download.Commit();
    scheduler.EndRendering(PassEnd::Buffer);
    const auto command = scheduler.Current().Handle();
    vk::MemoryBarrier before {};
    before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
    before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
        {}, 1, &before, 0, nullptr, 0, nullptr);
    uint64_t offset = 0;
    for (const auto& slice : slices) {
        const vk::BufferCopy copy {slice.offset, destination + offset, slice.copy_bytes};
        command.copyBuffer(slice.buffer, download.Handle(), 1, &copy);
        offset += slice.copy_bytes;
    }
    vk::MemoryBarrier after {};
    after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    after.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
        {}, 1, &after, 0, nullptr, 0, nullptr);
    scheduler.Wait(scheduler.CurrentTick());
    download.Invalidate(destination, total);
    result.reserve(payload);
    offset = 0;
    for (const auto& slice : slices) {
        const auto* first = static_cast<const uint8_t*>(mapped) + offset + slice.skip;
        result.insert(result.end(), first, first + slice.bytes);
        offset += slice.copy_bytes;
    }
    return true;
}

static DsIndirectProducerCapture BeginDsIndirectProducer(RenderContext& context,
    const PreparedBindings& bindings, const ShaderComputeInputInfo& input, uint64_t submit,
    vk::ShaderModule module, std::array<uint32_t, 3> groups, bool indirect = false,
    uint64_t args_address = 0, vk::Buffer args_buffer = {}, uint64_t args_offset = 0) {
    namespace Witness = DemonsIndirectWitness;
    DsIndirectProducerCapture cap;
    if (!Witness::Enabled() || !bindings.runtime || !bindings.runtime->program || !bindings.runtime->resources)
        return cap;
    const auto& program = *bindings.runtime->program;
    const auto& snapshot = *bindings.runtime->resources;
    const auto targets = Witness::TargetRanges();
    for (const auto target : targets) {
        bool candidate = false;
        vk::Buffer target_native {};
        uint64_t target_offset = 0;
        for (const auto& resource : program.info.buffers) {
            if (!resource.written || resource.descriptor_index >= bindings.buffer_sources.size()) continue;
            const auto slot = resource.descriptor_index;
            const auto& source = bindings.buffer_sources[slot];
            if (!DemonsIndirectWitnessPolicy::Overlap(source.address, source.size, target.address, target.bytes)) continue;
            candidate = true;
            const uint64_t word = uint64_t{program.bindings.memory_offset_dword} + slot / 4u;
            if (target_native || slot >= bindings.buffers.size() || word >= bindings.shader_data.size() ||
                target.address < source.address || target.address - source.address > source.size ||
                target.bytes > source.size - (target.address - source.address)) continue;
            const auto& native = bindings.buffers[slot];
            const uint64_t adjustment = (bindings.shader_data[word] >> ((slot % 4u) * 8u)) & 255u;
            const uint64_t relative = target.address - source.address;
            if (!native.buffer || native.range == VK_WHOLE_SIZE || adjustment > native.range ||
                relative > native.range - adjustment || target.bytes > native.range - adjustment - relative ||
                native.offset > UINT64_MAX - native.range) continue;
            target_native = native.buffer;
            target_offset = native.offset + adjustment + relative;
        }
        if (candidate) cap.streams.push_back({target, target_native, target_offset, {}, 0});
    }
    if (cap.streams.empty()) {
        // BDA can compute an address absent from every bound descriptor. This
        // is an explicit limitation, not a reason to read unrelated resources.
        static std::vector<std::pair<uint64_t, uint64_t>> unresolved;
        if (program.info.bda_writes) for (const auto target : targets) {
            const std::pair key {program.shader_hash, target.stream_id};
            if (std::ranges::find(unresolved, key) != unresolved.end()) continue;
            unresolved.push_back(key);
            Witness::Record({{"kind", "producer_unresolved"}, {"path", "compute_bda"},
                {"stream_id", target.stream_id}, {"address", target.address}, {"bytes", target.bytes},
                {"shader", program.shader_hash}, {"submit", submit},
                {"tick", context.GetCommandScheduler().CurrentTick()},
                {"reason", "bda_target_not_proven_no_readback"}, {"candidate_only", true}});
        }
        return cap;
    }
    const auto bound = ShaderDump::ModuleFile(module);
    const auto original = ShaderDump::ModuleOriginalFile(module);
    const auto guest = fmt::format("guest-shaders/cs_{:016x}_probe.bin", program.shader_hash);
    Witness::RegisterRequired(bound);
    Witness::RegisterRequired(original);
    Witness::RegisterRequired(guest);
    const auto recorded_groups = groups;
    bool groups_valid = true;
    if (args_buffer) groups_valid = ReadDsGpuWords(context, args_buffer, args_offset, groups, true);
    auto dispatch_groups = groups;
    if (!indirect) dispatch_groups[0] /= std::max(program.info.group_pack, 1u);
    cap.inputs = {{"shader", program.shader_hash}, {"shader_hex", fmt::format("{:016x}", program.shader_hash)},
        {"native_cs_file", bound}, {"original_cs_file", original}, {"guest_cs_file", guest},
        {"module_identity_complete", !bound.empty() && !original.empty()},
        {"frame", context.GetGpu().GetFrameNum()}, {"submit", submit},
        {"groups", groups}, {"groups_valid", groups_valid}, {"group_pack", program.info.group_pack},
        {"recorded_groups", recorded_groups}, {"dispatch_groups", dispatch_groups},
        {"groups_domain", indirect ? "native_dispatch" : "guest_before_group_pack"},
        {"threads", std::array{input.threads_num[0], input.threads_num[1], input.threads_num[2]}},
        {"dispatch_threads", std::array{input.dispatch_threads_num[0], input.dispatch_threads_num[1], input.dispatch_threads_num[2]}},
        {"wave_size", input.wave_size}, {"host_subgroup_size", input.host_subgroup_size},
        {"lds_size_dwords", input.lds_size_dwords}, {"scratch_size_dwords", input.scratch_size_dwords},
        {"float_mode", input.float_mode}, {"indirect", indirect}, {"args_guest", args_address},
        {"args_native_buffer", std::hash<VkBuffer>{}(static_cast<VkBuffer>(args_buffer))},
        {"args_native_offset", args_offset}, {"args_source", args_buffer ? "native_indirect" : "recorded_cpu_groups"},
        {"bda_writes", program.info.bda_writes}, {"uses_dma", program.info.uses_dma},
        {"bda_operands_complete", !program.info.uses_dma && !program.info.bda_writes},
        {"gds_operands_complete", !program.info.uses_gds},
        {"image_operands_complete", program.info.images.empty()},
        {"user_data", snapshot.user_data}, {"srt", snapshot.flattened_srt}, {"shader_data", bindings.shader_data},
        {"scalar_capture_complete", snapshot.diagnostic_scalar_complete},
        {"user_data_base", snapshot.diagnostic_user_data_base},
        {"scalar_regions", nlohmann::json::array()}, {"bindings", nlohmann::json::array()},
        {"images", nlohmann::json::array()}, {"samplers", nlohmann::json::array()},
        {"buffers_complete", true}, {"operand_tick_before", context.GetCommandScheduler().CurrentTick()},
        {"operand_mapping_epoch_before", context.GetBufferCache().UnregisterEpoch()},
        {"operand_gpu_dirty_generation_before", context.GetBufferCache().GpuDirtyGeneration()}};
    for (const auto& read : snapshot.diagnostic_scalar_reads)
        cap.inputs["scalar_regions"].push_back({{"address", read.address}, {"bytes", read.words.size() * 4u},
            {"read_kind", read.kind == 0 ? "host" : read.kind == 1 ? "specialization" : read.kind == 4 ? "ordinary" : "branch"},
            {"source", "consumed_srt"}, {"words", read.words}});
    for (const auto& descriptor : snapshot.images) cap.inputs["images"].push_back(descriptor.dwords);
    for (const auto& descriptor : snapshot.samplers) cap.inputs["samplers"].push_back(descriptor.dwords);

    uint64_t budget = Witness::GenerationBudget;
    for (auto& stream : cap.streams) {
        budget = std::min(budget, Witness::ProducerBlobCapacity(stream.target.stream_id));
        stream.before = DsIndirectProducerTarget(context, stream.target, stream.native, stream.offset);
    }
    cap.inputs["operand_budget_bytes"] = budget;
    std::vector<DsIndirectOperandSlice> slices;
    uint64_t used = 0;
    for (uint32_t slot = 0; slot < bindings.buffer_sources.size(); ++slot) {
        const auto& source = bindings.buffer_sources[slot];
        bool read = false, written = false, atomic = false;
        uint32_t resource_index = UINT32_MAX;
        for (uint32_t ordinal = 0; ordinal < program.info.buffers.size(); ++ordinal) {
            const auto& resource = program.info.buffers[ordinal];
            if (resource.descriptor_index != slot) continue;
            read |= resource.read; written |= resource.written; atomic |= resource.atomic;
            resource_index = ordinal;
        }
        nlohmann::json entry = {{"slot", slot}, {"address", source.address}, {"bytes", source.size},
            {"read", read}, {"written", written}, {"atomic", atomic}, {"captured_bytes", 0},
            {"capture_status", "not_read_by_shader"}};
        // Snapshot order is guest resource order, while native descriptor
        // slots place storage/chunks before UBOs and omit dead resources.
        if (resource_index < snapshot.buffers.size()) {
            entry["resource_index"] = resource_index;
            entry["descriptor"] = snapshot.buffers[resource_index].dwords;
        }
        if (slot < bindings.buffers.size()) {
            const auto& native = bindings.buffers[slot];
            entry["native_buffer"] = std::hash<VkBuffer>{}(static_cast<VkBuffer>(native.buffer));
            entry["native_offset"] = native.offset;
            entry["native_range"] = native.range;
            const uint64_t word = uint64_t{program.bindings.memory_offset_dword} + slot / 4u;
            if (word < bindings.shader_data.size())
                entry["byte_adjustment"] = (bindings.shader_data[word] >> ((slot % 4u) * 8u)) & 255u;
        }
        if ((read || atomic) && source.size) {
            entry["capture_status"] = "native_binding_missing";
            if (slot < bindings.buffers.size()) {
                const auto& native = bindings.buffers[slot];
                const uint64_t offset_word = uint64_t{program.bindings.memory_offset_dword} + slot / 4u;
                if (native.buffer && native.range != VK_WHOLE_SIZE && offset_word < bindings.shader_data.size()) {
                    const auto adjustment = (bindings.shader_data[offset_word] >> ((slot % 4u) * 8u)) & 255u;
                    entry["native_buffer"] = std::hash<VkBuffer>{}(static_cast<VkBuffer>(native.buffer));
                    entry["native_offset"] = native.offset;
                    entry["native_range"] = native.range;
                    entry["byte_adjustment"] = adjustment;
                    entry["capture_status"] = "native_bounds_or_alignment";
                    if (adjustment <= native.range && source.size <= native.range - adjustment &&
                        native.offset <= UINT64_MAX - native.range && source.size <= UINT64_MAX - 6u) {
                        const uint64_t start = native.offset + adjustment;
                        const uint64_t skip = start & 3u;
                        const uint64_t length = (skip + source.size + 3u) & ~uint64_t{3};
                        if ((start & ~uint64_t{3}) >= native.offset && length <= native.range - adjustment + skip) {
                            if (source.size > budget - used) entry["capture_status"] = "generation_budget_exceeded";
                            else {
                                entry["capture_status"] = "pending_batch";
                                entry["offset_bytes"] = used;
                                slices.push_back({native.buffer, start & ~uint64_t{3}, length, skip, source.size,
                                    cap.inputs["bindings"].size()});
                                used += source.size;
                            }
                        }
                    }
                }
            }
        } else if (!source.size) entry["capture_status"] = "empty";
        cap.inputs["bindings"].push_back(std::move(entry));
    }
    const bool read_ok = ReadDsIndirectOperandSlices(context, slices, cap.operand_bytes);
    for (const auto& slice : slices) {
        auto& entry = cap.inputs["bindings"][slice.binding];
        entry["capture_status"] = read_ok ? "complete" : "native_read_failed";
        entry["captured_bytes"] = read_ok ? slice.bytes : 0;
    }
    for (const auto& entry : cap.inputs["bindings"])
        if ((entry["read"].get<bool>() || entry["atomic"].get<bool>()) && entry["bytes"].get<uint64_t>() &&
            entry["capture_status"] != "complete") cap.inputs["buffers_complete"] = false;
    cap.inputs["operand_tick_after"] = context.GetCommandScheduler().CurrentTick();
    cap.inputs["operand_mapping_epoch_after"] = context.GetBufferCache().UnregisterEpoch();
    cap.inputs["operand_gpu_dirty_generation_after"] = context.GetBufferCache().GpuDirtyGeneration();
    cap.inputs["operand_blob_bytes"] = cap.operand_bytes.size();
    cap.inputs["operand_blob_fnv1a64"] = Witness::Digest(cap.operand_bytes);
    for (auto& stream : cap.streams) {
        auto event = cap.inputs;
        event["kind"] = "producer"; event["phase"] = "before";
        event["path"] = "compute_descriptor_candidate"; event["candidate_only"] = true;
        event["stream_id"] = stream.target.stream_id;
        event["epoch"] = Witness::Epoch(stream.target.stream_id);
        event["target"] = stream.before;
        stream.before_seq = Witness::Record(std::move(event));
        Witness::Mutation(stream.target.address, stream.target.bytes, "compute_descriptor_candidate");
    }
    return cap;
}

static void EndDsIndirectProducer(RenderContext& context, DsIndirectProducerCapture&& cap) {
    namespace Witness = DemonsIndirectWitness;
    static std::vector<uint64_t> retained_streams;
    for (const auto& stream : cap.streams) {
        auto after = DsIndirectProducerTarget(context, stream.target, stream.native, stream.offset);
        const bool complete = stream.before.contains("native_words") && after.contains("native_words");
        const bool changed = complete && stream.before["native_words"] != after["native_words"];
        const bool first = std::ranges::find(retained_streams, stream.target.stream_id) == retained_streams.end();
        bool retained = false;
        nlohmann::json artifact = {{"status", "not_retained_unchanged"}};
        if (changed || first) {
            Witness::BeginProducer(stream.target.stream_id);
            const auto name = fmt::format("ds_prelight_cp_args_current_{}_compute.bin", stream.target.stream_id);
            retained = cap.operand_bytes.empty() || Witness::SaveBlob(name, cap.operand_bytes);
            artifact = {{"status", retained ? "retained" : "blob_budget_or_io_error"},
                {"bytes", cap.operand_bytes.size()}, {"fnv1a64", Witness::Digest(cap.operand_bytes)}};
            // A current name is mutable provenance. Only the header's pinned
            // files/required manifest are file dependencies with exact length.
            if (!cap.operand_bytes.empty()) artifact["artifact_name"] = "queue-state/" + name;
            if (retained && first) retained_streams.push_back(stream.target.stream_id);
        }
        const bool same_mapping = complete && stream.before["native_buffer"] == after["native_buffer"] &&
            stream.before["native_offset"] == after["native_offset"] &&
            stream.before["mapping_epoch_after"] == after["mapping_epoch_before"];
        Witness::Record({{"kind", "producer"}, {"phase", "after"},
            {"path", "compute_descriptor_candidate"}, {"candidate_only", true},
            {"stream_id", stream.target.stream_id}, {"before_seq", stream.before_seq},
            {"epoch", Witness::Epoch(stream.target.stream_id)},
            {"shader", cap.inputs["shader"]}, {"submit", cap.inputs["submit"]},
            {"frame", cap.inputs["frame"]}, {"target_before", stream.before}, {"target_after", after},
            {"target_bytes_comparable", complete}, {"target_bytes_changed", changed},
            {"same_native_mapping", same_mapping}, {"retained", retained}, {"operand_blob", artifact},
            {"inputs", retained ? cap.inputs : nlohmann::json{}},
            {"scope", "observed before/after dispatch; descriptor overlap alone does not prove a store or last writer"}});
    }
}

struct DsVertexProducerCapture{
    nlohmann::json report;
    std::vector<DemonsPrelightTrace::VertexRange> targets;
};
static nlohmann::json DsVertexRangeWords(RenderContext& context,DemonsPrelightTrace::VertexRange r){
    nlohmann::json out={{"address",r.address},{"bytes",r.bytes},{"confirmed",r.confirmed}};
    auto& cache=context.GetBufferCache();
    out["writers"]=nlohmann::json::array();
    for(const auto& w:cache.RecentProbeRayTransfers(r.address,r.bytes))if(w.path)
        out["writers"].push_back({{"path",w.path},{"tick",w.tick},{"shader",w.shader},{"begin",w.begin},{"end",w.end},{"source",w.source}});
    auto* b=cache.PeekVertexProducerBuffer(r.address,r.bytes);
    if(b&&r.bytes%4==0&&r.bytes<=65536){
        out["gpu_dirty"]=cache.HasGpuDirtyBytes(r.address,r.bytes);
        auto words=DemonsPrelightTrace::ReadNativeWords(context,b->Handle(),b->Offset(r.address),uint32_t(r.bytes/4));
        out["nan_words"]=std::ranges::count_if(words,DemonsPrelightTrace::IsNanWord);
        out["word_digest"]=DemonsPrelightTrace::VertexWordDigest(words);
        out["captured_words"]=words.size();
        out["native_buffer"]=std::hash<VkBuffer>{}(static_cast<VkBuffer>(b->Handle()));
        out["native_offset"]=b->Offset(r.address);
        out["gpu_words"]=std::move(words);
    }
    return out;
}
static void SaveDsVertexCpuSource(int32_t frame,uint64_t submit,uint64_t tick,const nlohmann::json& source){
    using namespace DemonsPrelightTrace;
    if(!source.value("complete",false))return;
    const auto address=source.value("address",0ull);
    const auto bytes=source.value("bytes",0ull);
    const auto folded=Kernel::AddressFold::Fold(address);
    nlohmann::json report=source;
    report.erase("gpu_words");report.erase("backing_words");
    report["frame"]=frame;report["submit"]=submit;report["tick"]=tick;
    report["kind"]="xyz_cpu_source";
    report["fault_candidates"]=nlohmann::json::array();
    const auto faults=Cpu::DemonsGuestWriteHistory::Overlap(address,bytes);
    report["fault_ring_oldest"]=faults.oldest;report["fault_ring_newest"]=faults.newest;
    report["fault_candidates_total"]=faults.events.size();report["fault_candidates_truncated"]=faults.truncated;
    std::vector<uint64_t> emitted;
    for(int pass=0;pass<3;++pass){
        unsigned count=0;
        for(const auto& e:faults.events){
            const bool exact=e.address>=folded&&e.address-folded<bytes;
            const bool copy_candidate=exact&&e.evidence.words[17]!=0;
            if((pass==0&&!copy_candidate)||(pass==1&&!exact)||(pass==2&&exact)||
                count>=static_cast<unsigned>(pass==0?8:pass==1?2:1)||std::ranges::find(emitted,e.id)!=emitted.end())continue;
            emitted.push_back(e.id);
            nlohmann::json row={{"event",e.id},{"fault_address",e.address},{"guest_rip",e.rip},
                {"exact_range",exact},{"gpr",e.gpr},{"rsi",e.rsi},{"rdi",e.rdi},{"rcx",e.rcx}};
            const auto& w=e.evidence.words;
            row["fault_code_read_bytes"]=w[0];
            if(w[0]){
                std::array<uint8_t,32> code{};
                std::memcpy(code.data(),w.data()+1,code.size());
                row["code_base"]=e.rip;
                row["code_bytes"]=std::vector<uint8_t>(code.begin(),code.begin()+std::min<uint64_t>(w[0],code.size()));
            }
            row["stack_read_bytes"]=w[5];
            if(w[5])row["stack_words"]=std::vector<uint64_t>(w.begin()+6,w.begin()+6+std::min<uint64_t>(w[5]/8,8));
            if(w[18]){
                std::array<uint32_t,4> xmm{},prefix{},dest{};
                std::memcpy(xmm.data(),w.data()+19,16);
                std::memcpy(prefix.data(),w.data()+21,16);
                row["xmm0_words"]=xmm;row["xmm0_host_register"]=w[26];
                row["host_pc"]=w[24];row["host_opcode"]=w[25];
                if(w[23]==16){row["rsi_prefix"]=prefix;row["xmm0_matches_rsi"]=xmm==prefix;}
                if(exact&&e.address%4==0&&e.address-folded+16<=bytes&&source.contains("gpu_words")){
                    const auto offset=(e.address-folded)/4;
                    if(source["gpu_words"].size()>=offset+4){
                        for(size_t j=0;j<4;++j)dest[j]=source["gpu_words"][offset+j].get<uint32_t>();
                        row["consumed_store_words"]=dest;row["xmm0_matches_consumer"]=xmm==dest;
                    }
                }
            }
            if(w[28]){
                std::array<uint8_t,64> caller{};
                std::memcpy(caller.data(),w.data()+29,64);
                row["caller_code_base"]=w[27];
                row["caller_code_bytes"]=std::vector<uint8_t>(caller.begin(),caller.begin()+std::min<uint64_t>(w[28],caller.size()));
            }
            if(w[17]){
                row["source_at_fault"]=w[17];row["source_read_bytes"]=w[14];
                row["source_nan_words"]=w[16];row["source_digest"]=w[15];
                if(exact&&source.contains("gpu_words")&&w[14]&&w[15]){
                    const auto offset=e.address-folded;
                    const auto page_bytes=std::min<uint64_t>(4096,bytes-offset);
                    if(w[14]==page_bytes&&offset%4==0&&source["gpu_words"].size()>=(offset+page_bytes)/4){
                        uint64_t digest=14695981039346656037ull,nans=0;
                        for(uint64_t i=offset/4;i<(offset+page_bytes)/4;++i){
                            const auto x=source["gpu_words"][i].get<uint32_t>();
                            digest^=x;digest*=1099511628211ull;nans+=IsNanWord(x);
                        }
                        row["destination_page_digest"]=digest;row["destination_page_nan_words"]=nans;
                        row["source_matches_destination_digest"]=digest==w[15];
                    }
                }
            }
            report["fault_candidates"].push_back(std::move(row));++count;
        }
    }
    report["file_read_candidates"]=nlohmann::json::array();
    for(const auto& e:LibKernel::DemonsFileReadHistory::Overlap(address,bytes))
        report["file_read_candidates"].push_back({{"event",e.id},{"begin",e.begin},{"end",e.end},
            {"file_offset",e.file_offset},{"path",e.path}});
    static std::mutex mutex;
    static std::ofstream out;
    static uint64_t events=0,used=0,dropped=0;
    std::scoped_lock lock(mutex);
    report["seq"]=++events;
    if(events==1){
        std::error_code ec;std::filesystem::create_directories(Folder(),ec);
        out.open(Folder()/"ds_vertex_producer_cpu_source.jsonl",std::ios::trunc);
    }
    const auto line=report.dump();
    if(!out||used+line.size()+1>8u*1024u*1024u)++dropped;
    else{out<<line<<'\n';out.flush();if(out)used+=line.size()+1;else ++dropped;}
    std::ofstream status(Folder()/"ds_vertex_producer_cpu_status.json",std::ios::trunc);
    if(status)status<<nlohmann::json{{"events",events},{"bytes",used},{"dropped",dropped},
        {"scope","CPU first-write fault candidates, not complete store history"}};
    static uint64_t last=UINT64_MAX;
    static unsigned retained=0;
    const auto n=source.value("nan_words",0ull);
    if(n!=last&&retained<16){
        last=n;++retained;
        report["gpu_words"]=source["gpu_words"];
        if(source.contains("backing_words"))report["backing_words"]=source["backing_words"];
        std::ofstream edge(Folder()/fmt::format("ds_vertex_producer_cpu_edge_f{}_s{}_t{}.json",frame,submit,tick),std::ios::trunc);
        if(edge)edge<<report;
    }
}
[[maybe_unused]] static DsVertexProducerCapture BeginDsVertexProducer(RenderContext& context,const PreparedBindings& bindings,
    uint64_t submit,vk::ShaderModule module,std::array<uint32_t,3> groups,bool indirect,uint64_t args_addr){
    using namespace DemonsPrelightTrace;
    DsVertexProducerCapture cap;
    if(!Enabled()||!bindings.runtime||!bindings.runtime->program||!bindings.runtime->resources)return cap;
    const auto& prog=*bindings.runtime->program;
    const auto frame=context.GetGpu().GetFrameNum();
    const int phase=std::clamp(Phase(frame),0,3);
    const bool learned=VertexShader(prog.shader_hash);
    const bool xyz_copy=prog.shader_hash==0x22e0328668b00aa3ull &&
        bindings.buffer_sources.size()>2&&bindings.buffer_sources[1].size==31488&&
        bindings.buffer_sources[2].size==31488&&groups[0]==123;
    // #734: source CPU-owned for this exact copy; stop capturing unrelated producers.
    if(!xyz_copy)return cap;
    if(xyz_copy){
        const auto& src=bindings.buffer_sources[1];
        WatchVertexInput(src.address,src.size);
        const auto* writer=FindDsBufferProducer(frame,src.address,src.size,false);
        if(writer)WatchVertexShader(writer->shader);
    }
    for(auto r:VertexRanges()){
        bool match=false;
        for(const auto& res:prog.info.buffers)if(res.written&&res.descriptor_index<bindings.buffer_sources.size()){
            const auto& b=bindings.buffer_sources[res.descriptor_index];
            if(DemonsPrelightPolicy::Overlaps(b.address,b.size,r.address,r.bytes))match=true;
        }
        // Only XYZ, not packed auxiliary words. Unconfirmed session seeds
        // cannot consume the repeated capture budget.
        if(r.confirmed&&r.bytes==31488&&(match||(!r.upstream&&learned&&prog.info.bda_writes)))cap.targets.push_back(r);
    }
    if(cap.targets.empty()&&!xyz_copy)return cap;
    static std::mutex capture_mutex;
    static uint64_t taken=0,copy_taken=0;
    {
        std::scoped_lock lock(capture_mutex);
        auto& budget=xyz_copy?copy_taken:taken;
        if(budget>=768){
            VertexRecord({{"kind","dispatch_capture_skipped"},{"reason","capture_budget"},{"frame",frame},{"submit",submit},{"shader",prog.shader_hash}});
            cap.targets.clear();return cap;
        }
        ++budget;
    }
    const auto& snap=*bindings.runtime->resources;
    cap.report={{"frame",frame},{"submit",submit},{"phase",phase},{"shader",prog.shader_hash},
        {"shader_hex",fmt::format("{:016x}",prog.shader_hash)},{"native_cs_file",ShaderDump::ModuleFile(module)},
        {"groups",groups},{"indirect",indirect},{"args_guest",args_addr},
        {"tick_before",context.GetCommandScheduler().CurrentTick()},{"xyz_copy",xyz_copy},{"srt",snap.flattened_srt},
        {"user_data",snap.user_data},{"learned_writer",learned},{"bindings",nlohmann::json::array()},
        {"target_before",nlohmann::json::array()},{"target_after",nlohmann::json::array()}};
    size_t remaining=32768;
    for(uint32_t slot=0;slot<bindings.buffer_sources.size()&&slot<bindings.buffers.size();++slot){
        const auto& b=bindings.buffer_sources[slot];
        bool written=false;
        for(const auto& res:prog.info.buffers)if(res.descriptor_index==slot)written|=res.written;
        nlohmann::json entry={{"slot",slot},{"address",b.address},{"bytes",b.size},{"written",written}};
        if(slot<snap.buffers.size())entry["descriptor"]=snap.buffers[slot].dwords;
        const auto n=std::min<size_t>({size_t(b.size/4),size_t(8192),remaining});
        if(n){
            auto words=ReadDsBoundWords(context,bindings,slot,n);
            remaining-=std::min(remaining,words.size());
            entry["captured_words"]=words.size();entry["complete"]=words.size()*4==b.size;
            if(xyz_copy&&slot==1){
                entry["nan_words"]=std::ranges::count_if(words,IsNanWord);
                entry["word_digest"]=VertexWordDigest(words);
                entry["gpu_dirty"]=context.GetBufferCache().HasGpuDirtyBytes(b.address,b.size);
                entry["writers"]=nlohmann::json::array();
                for(const auto& w:context.GetBufferCache().RecentProbeRayTransfers(b.address,b.size))if(w.path)
                    entry["writers"].push_back({{"path",w.path},{"tick",w.tick},{"shader",w.shader},{"begin",w.begin},{"end",w.end},{"source",w.source}});
                if(const auto* p=FindDsBufferProducer(frame,b.address,b.size,false))
                    entry["compute_candidate"]={{"shader",p->shader},{"frame",p->frame},{"submit",p->submit},{"address",p->address},{"bytes",p->size}};
                entry["staging_history"]=VertexUploadHistory(b.address,b.size);
                std::vector<uint32_t> cpu(n);
                const bool ok=LibKernel::Memory::TryReadBacking(b.address,cpu.data(),cpu.size()*4);
                entry["backing_ok"]=ok;
                if(ok){
                    entry["backing_nan_words"]=std::ranges::count_if(cpu,IsNanWord);
                    entry["backing_digest"]=VertexWordDigest(cpu);
                    entry["backing_equals_gpu"]=cpu==words;
                    entry["backing_words"]=std::move(cpu);
                }
            }
            entry["gpu_words"]=std::move(words);
            if(xyz_copy&&slot==1)SaveDsVertexCpuSource(frame,submit,context.GetCommandScheduler().CurrentTick(),entry);
        }
        cap.report["bindings"].push_back(std::move(entry));
    }
    for(auto r:cap.targets)cap.report["target_before"].push_back(DsVertexRangeWords(context,r));
    return cap;
}
static void EndDsVertexProducer(RenderContext& context,DsVertexProducerCapture&& cap){
    if(cap.report.is_null())return;
    for(auto r:cap.targets)cap.report["target_after"].push_back(DsVertexRangeWords(context,r));
    cap.report["tick_after"]=context.GetCommandScheduler().CurrentTick();
    // Preserve every transition, but only the first unchanged dispatch per
    // shader/target. Emit compact before/after observations for all captures.
    static std::mutex retained_mutex;
    static std::vector<std::pair<uint64_t,uint64_t>> retained;
    bool keep=false,edge=false;
    {
        std::scoped_lock lock(retained_mutex);
        const uint64_t shader=cap.report["shader"].get<uint64_t>();
        for(size_t i=0;i<cap.targets.size();++i){
            const auto& a=cap.report["target_before"][i];
            const auto& b=cap.report["target_after"][i];
            const auto expected=cap.targets[i].bytes/4;
            const auto before=a.value("captured_words",0ull);
            const auto after=b.value("captured_words",0ull);
            if(before==expected&&after==expected&&
                a.value("nan_words",0ull)!=b.value("nan_words",0ull))edge=true;
            const std::pair key{shader,cap.targets[i].address};
            if(retained.size()<24&&std::ranges::find(retained,key)==retained.end()){
                retained.push_back(key);keep=true;
            }
        }
    }
    nlohmann::json summary=cap.report;
    if(cap.report.value("xyz_copy",false)&&cap.report["bindings"].size()>1){
        auto src=cap.report["bindings"][1];
        src.erase("gpu_words");src.erase("backing_words");
        summary["xyz_source"]=std::move(src);
        // Capture the first finite/partial/all-NaN source state, including
        // a source seen before the corresponding VS destination was learned.
        static std::mutex source_mutex;
        static std::vector<std::pair<int,int>> source_states;
        const auto& s=summary["xyz_source"];
        if(s.value("complete",false)){
            const auto klass=DemonsPrelightTrace::VertexNanClass(s.value("nan_words",0ull),s.value("captured_words",0ull));
            const int backing=!s.value("backing_ok",false)?0:(s.value("backing_equals_gpu",false)?1:2);
            const std::pair<int,int> key{klass,backing};
            std::scoped_lock lock(source_mutex);
            if(std::ranges::find(source_states,key)==source_states.end()){
                source_states.push_back(key);keep=true;
            }
        }
    }
    summary.erase("bindings");summary.erase("srt");summary.erase("user_data");
    for(auto key:{"target_before","target_after"})for(auto& e:summary[key])e.erase("gpu_words");
    summary["kind"]="dispatch_xyz";summary["nan_count_changed"]=edge;
    summary["retained_full"]=keep||edge;
    DemonsPrelightTrace::VertexRecord(std::move(summary));
    if(!keep&&!edge)return;
    cap.report["nan_count_changed"]=edge;
    const auto file=fmt::format("ds_vertex_producer_{}_f{}_s{}_t{}.json",
        cap.report["shader_hex"].get<std::string>(),cap.report["frame"].get<int32_t>(),
        cap.report["submit"].get<uint64_t>(),cap.report["tick_before"].get<uint64_t>());
    const auto folder=DemonsPrelightTrace::Folder();std::error_code ec;std::filesystem::create_directories(folder,ec);
    std::ofstream out(folder/file,std::ios::trunc);if(out)out<<cap.report;
    out.close();
    DemonsPrelightTrace::Record({{"kind","vertex_producer_capture"},{"file",file},{"shader",cap.report["shader"]},
        {"frame",cap.report["frame"]},{"submit",cap.report["submit"]},{"saved",out.good()}});
}

// The guest binaries for these two writers contain a typed count load at
// 0x28, per-object flag checks, then buffer_atomic_add into the count table.
// Capture real inputs BEFORE execution and output AFTER the same dispatch.
// Never overwrite shader values or force an active lane.
static bool DsCountProducer(uint64_t hash) {
    return hash == 0x3b7e54e11ee65120ull || hash == 0xfb18d110fab8234eull ||
           hash == 0x2454ae3e3a6cbdabull || hash == 0xb16650e4c9acfdddull ||
           hash == 0xd45e1b87d6741f63ull;
}

// Read a checked slice of the actual native descriptor, including its push
// data alignment adjustment. A 5.4 MB list needs at most 64 records here.
static std::vector<uint32_t> ReadDsCountSlice(RenderContext& context,
    const PreparedBindings& bindings,uint32_t slot,uint64_t first_word,size_t count) {
    if (slot>=bindings.buffers.size() || slot>=bindings.buffer_sources.size() || !count) return {};
    const auto& native=bindings.buffers[slot];
    const auto& source=bindings.buffer_sources[slot];
    const auto& program=*bindings.runtime->program;
    const auto offset_word=program.bindings.memory_offset_dword+slot/4u;
    if (offset_word>=bindings.shader_data.size() || !native.buffer || native.range==VK_WHOLE_SIZE ||
        first_word>source.size/4 || count>source.size/4-first_word) return {};
    const uint64_t adjust=(bindings.shader_data[offset_word]>>((slot%4u)*8u))&255u;
    const uint64_t offset=first_word*4;
    if (adjust>native.range || offset>native.range-adjust ||
        count>(native.range-adjust-offset)/4) return {};
    std::vector<uint32_t> words(count);
    if (!ReadDsGpuWords(context,native.buffer,native.offset+adjust+offset,words)) words.clear();
    return words;
}

// 195657: native PC 0x64 of fb18 reads DWORD zero of a 12-byte list
// record at flat SRT[64]+lane; PC 0x70 reads flags at object+208.
// The translated module addresses binding 0 slots 1 and 2 in the same way.
// These are expected guest gate inputs, not an assertion that the GPU took it.
static void CaptureDsCountFirstGate(RenderContext& context,const PreparedBindings& bindings,
                                   nlohmann::json& report) {
    const auto& snap=*bindings.runtime->resources;
    if (bindings.runtime->program->shader_hash!=0xfb18d110fab8234eull ||
        snap.flattened_srt.size()<=64 || snap.buffers.size()<3) return;
    const auto& before=report["before"];
    if (before.size()<3 || !before[0].contains("gpu_words") ||
        before[0]["gpu_words"].size()<4 || !before[2].contains("gpu_words")) return;
    const auto count=before[0]["gpu_words"][3].get<uint32_t>();
    const auto base=snap.flattened_srt[64];
    const auto list_desc=DecodeNativeDescriptor<ShaderBufferResource>(snap.buffers[1]);
    const auto obj_desc=DecodeNativeDescriptor<ShaderBufferResource>(snap.buffers[2]);
    nlohmann::json gate={{"count",count},{"list_base_index",base},
        {"list_stride",list_desc.Stride()},{"object_stride",obj_desc.Stride()},
        {"status","layout_mismatch"}};
    if (list_desc.Stride()!=12 || obj_desc.Stride()!=220 || count>64) {
        report["first_gate"]=std::move(gate); return;
    }
    if (!count) { gate["status"]="zero_count"; report["first_gate"]=std::move(gate); return; }
    const auto first_word=uint64_t{base}*3;
    const auto list=ReadDsCountSlice(context,bindings,1,first_word,size_t{count}*3);
    const auto& source=bindings.buffer_sources[1];
    gate["list_address"]=source.address+first_word*4;
    gate["list_words"]=list;
    gate["status"]=list.size()==size_t{count}*3?"captured":"list_read_failed_or_out_of_range";
    if (list.size()==size_t{count}*3) {
        std::vector<uint32_t> backing(list.size());
        if (LibKernel::Memory::TryReadBacking(source.address+first_word*4,backing.data(),backing.size()*4))
            gate["list_backing_words"]=backing;
        const auto& objects=before[2]["gpu_words"];
        auto rows=nlohmann::json::array();
        for (uint32_t lane=0;lane<count;++lane) {
            const uint32_t id=list[3u*lane];
            const uint64_t flag_word=uint64_t{id}*55u+52u;
            const auto addr=source.address+(first_word+3ull*lane)*4;
            nlohmann::json row={{"lane",lane},{"list_index",uint64_t{base}+lane},
                {"object_id",id},{"list_word_address",addr},{"flag_word",flag_word},
                {"flag_in_bounds",flag_word<objects.size()}};
            if (flag_word<objects.size()) {
                const auto flags=objects[flag_word].get<uint32_t>();
                row["flags"]=flags;
                row["expected_pass_flag_gate"]=(flags&8u)==0;
                if (before[2].contains("backing_words") && flag_word<before[2]["backing_words"].size()) {
                    const auto backing_flags=before[2]["backing_words"][flag_word].get<uint32_t>();
                    row["backing_flags"]=backing_flags;
                    row["flags_match_backing"]=flags==backing_flags;
                }
            }
            if (const auto* w=FindDsBufferProducer(report["frame"].get<int32_t>(),addr,4,false))
                row["list_writer_candidate"]={{"shader",fmt::format("{:016x}",w->shader)},
                    {"frame",w->frame},{"submit",w->submit},{"address",w->address},{"bytes",w->size}};
            rows.push_back(std::move(row));
        }
        gate["lanes"]=std::move(rows);
    }
    report["first_gate"]=std::move(gate);
}

static std::optional<nlohmann::json> BeginDsCountProducer(
    RenderContext& context, const PreparedBindings& bindings, uint64_t submit) {
    namespace D=DemonsDominantGeometry;
    if (!DsTraceEnabled() || Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return {};
    const auto& program=*bindings.runtime->program;
    const bool upstream_count=program.shader_hash==0xcb3b9c1ea64fe876ull;
    const auto& snap=*bindings.runtime->resources;
    const bool secondary_active=upstream_count && DemonsCausalMatrix::HasSecondary(snap.flattened_srt);
    uint64_t watch_address=0;
    if (upstream_count) {
        // The exact 20-byte dispatch-count records from all 21 195657 samples.
        constexpr std::array<uint64_t,3> watched {0x20817f7c0ull,0x20817dfc0ull,0x20817daa0ull};
        for (const auto& res:program.info.buffers) {
            if (!res.written || res.descriptor_index>=bindings.buffer_sources.size()) continue;
            const auto& src=bindings.buffer_sources[res.descriptor_index];
            for (auto address:watched)
                if (D::Overlaps(src.address,src.size,address,20)) watch_address=address;
        }
        // Separate the live-secondary path from specialized zero-secondary
        // captures. Watch the actual output address, not only old addresses.
        if(secondary_active && DemonsCausalMatrix::Evidence()){
            const uint64_t address=uint64_t{snap.flattened_srt[10]}|
                (uint64_t{snap.flattened_srt[11]&0xffffu}<<32u);
            if(address) for(const auto& res:program.info.buffers){
                if(!res.written||res.descriptor_index>=bindings.buffer_sources.size())continue;
                const auto& src=bindings.buffer_sources[res.descriptor_index];
                if(src.address==address&&src.size>=20)watch_address=address;
            }
        }
        if (!watch_address) return {};
    } else {
        if (!DsCountProducer(program.shader_hash) || bindings.buffer_sources.size()<11 ||
            bindings.buffers.size()<11) return {};
        const auto& count=bindings.buffer_sources[7];
        if (count.address!=D::CountInputAddress || count.size!=D::CountInputWords*4u) {
            if (!DemonsCausalMatrix::Evidence() || program.shader_hash!=0xfb18d110fab8234eull ||
                !count.address || !count.size || count.size>131072 || count.size%4) return {};
        }
    }
    const auto frame=context.GetGpu().GetFrameNum();
    const auto& gate=DemonsAdaptiveCapture::Current();
    const uint32_t phase=gate.PostTransitionDarkDue(frame)?3u:gate.DarkOnsetDue(frame)?2u:
        gate.BrightSceneActive(frame)?1u:0u;
    struct Gate { std::array<int32_t,4> last{-1,-1,-1,-1}; std::array<uint32_t,4> count{}; std::array<uint32_t,4> same_frame{}; };
    static std::unordered_map<uint64_t,Gate> seen;
    auto& state=seen[secondary_active ? program.shader_hash^0x5345434f4e444152ull :
        upstream_count ? program.shader_hash^watch_address : program.shader_hash];
    // Reserve a separate budget for dark onset and post-transition;
    // early scene sampling cannot consume the late capture.
    const uint32_t budget=secondary_active ? (phase==0?12u:phase==3?12u:8u) :
        upstream_count ? (phase==0?4u:phase==3?4u:2u) :
        (phase==3 && (program.shader_hash==0x3b7e54e11ee65120ull ||
                      program.shader_hash==0xfb18d110fab8234eull) ? 8u : phase==0?4u:2u);
    if (state.count[phase]>=budget) return {};
    if (state.last[phase]==frame) {
        // cb3b can populate the same dispatch record twice in one frame
        // (zero for the first consumer, nonzero for the second).
        if (!upstream_count || state.same_frame[phase]>=(secondary_active?2u:4u)) return {};
    } else {
        if (state.last[phase]>=0 && frame-state.last[phase]<(secondary_active?8:24)) return {};
        state.last[phase]=frame;
        state.same_frame[phase]=0;
    }
    ++state.same_frame[phase];
    ++state.count[phase];
    static uint32_t capture_serial=0;
    const auto seq=++capture_serial;
    nlohmann::json report={{"frame",frame},{"submit",submit},{"phase",phase},
        {"shader",fmt::format("{:016x}",program.shader_hash)},{"srt",snap.flattened_srt},
        {"watch_address",watch_address},{"capture_seq",seq},{"secondary_active",secondary_active},
        {"user_data",snap.user_data},{"shader_data",bindings.shader_data},
        {"matrix_mode",static_cast<unsigned>(DemonsCausalMatrix::Mode())},
        {"execution_probe_variant",bindings.runtime->program->ds_light_math_variant},
        {"capture_tick_before",context.GetCommandScheduler().CurrentTick()}};
    auto buffers=nlohmann::json::array();
    for (uint32_t slot=0;slot<bindings.buffer_sources.size() && slot<bindings.buffers.size();++slot) {
        const auto& src=bindings.buffer_sources[slot];
        nlohmann::json entry={{"slot",slot},{"guest",src.address},{"bytes",src.size}};
        if (src.address && src.size && (slot==0 || slot==1 || slot==2 || slot==7)) {
            entry["gpu_dirty"]=context.GetBufferCache().HasGpuDirtyBytes(src.address,src.size);
            entry["cpu_modified"]=context.GetBufferCache().IsRegionCpuModified(src.address,src.size);
        }
        if (slot<snap.buffers.size()) entry["descriptor"]=snap.buffers[slot].dwords;
        entry["native_offset"]=bindings.buffers[slot].offset;
        entry["native_range"]=bindings.buffers[slot].range;
        // Small model tables, per-object lists, flags and reservation counters
        // are captured in full. Large vertex payloads are not needed for the
        // first gate; report explicitly that they were not read.
        if (src.size && src.size<=131072 && src.size%4==0) {
            auto words=ReadDsBoundWords(context,bindings,slot,src.size/4);
            entry["status"]=words.size()==src.size/4?"captured":"incomplete_gpu_read";
            entry["gpu_words"]=std::move(words);
            if (slot==0 || slot==7 || upstream_count || (!upstream_count && slot==2) ||
                (upstream_count && D::Overlaps(src.address,src.size,watch_address,20))) {
                std::vector<uint32_t> backing(src.size/4);
                if (LibKernel::Memory::TryReadBacking(src.address,backing.data(),src.size))
                    entry["backing_words"]=std::move(backing);
            }
        } else {
            entry["status"]="not_captured_large_or_invalid";
            if (upstream_count && watch_address>=src.address &&
                watch_address-src.address<=src.size && 20<=src.size-(watch_address-src.address) &&
                (watch_address-src.address)%4==0) {
                entry["watched_words"]=ReadDsCountSlice(context,bindings,slot,(watch_address-src.address)/4,5);
                entry["watched_address"]=watch_address;
            }
        }
        if (const auto* w=FindDsBufferProducer(frame,src.address,src.size,false))
            entry["overlapping_compute_candidate"]={{"shader",fmt::format("{:016x}",w->shader)},
                {"frame",w->frame},{"submit",w->submit},{"address",w->address},{"bytes",w->size},{"fill",w->fill},{"serial",w->serial}};
        buffers.push_back(std::move(entry));
    }
    report["before"]=std::move(buffers);
    CaptureDsCountFirstGate(context,bindings,report);
    report["images"]=nlohmann::json::array();
    for (const auto& desc:snap.images) report["images"].push_back(desc.dwords);
    report["samplers"]=nlohmann::json::array();
    for (const auto& desc:snap.samplers) report["samplers"].push_back(desc.dwords);
    return report;
}

static void EndDsCountProducer(RenderContext& context,const PreparedBindings& bindings,
    std::optional<nlohmann::json> report,std::array<uint32_t,3> groups,
    vk::Buffer indirect={},uint64_t indirect_offset=0) {
    if (!report) return;
    namespace P=DemonsCountExecProbe;
    if(bindings.runtime->program->shader_hash==P::Shader &&
       P::SupportedFingerprint(bindings.runtime->program->ds_light_math_variant)){
        std::vector<uint32_t> words(P::WordCount);
        const auto& native=bindings.shared_memory;
        // The shader writes only the dedicated debug descriptor. Readback
        // is from the exact sampled dispatch, before any later producer.
        if(native.buffer && native.range==words.size()*4ull &&
           ReadDsGpuWords(context,native.buffer,native.offset,words)){
            const auto word=[&](uint32_t lane,uint32_t site)->std::optional<uint64_t>{
                const auto it=std::lower_bound(P::Sites.begin(),P::Sites.end(),site);
                if(it==P::Sites.end()||*it!=site)return {};
                const auto offset=1+(lane*P::Sites.size()+size_t(it-P::Sites.begin()))*P::WordsPerSite;
                if(!words[offset])return {};
                return uint64_t{words[offset+1]}|(uint64_t{words[offset+2]}<<32);
            };
            auto bda=nlohmann::json::array();
            // The first two raw loads at native PC 0x190 are generated
            // from these address, resolved-pointer and load triplets.
            constexpr std::array<std::array<uint32_t,3>,6> sites{{
                {931,932,938},{945,946,951},{971,972,977},{984,985,990},
                {6490,6491,6496},{6503,6504,6509}}};
            for(uint32_t lane=0;lane<P::Lanes;++lane){
                if(word(lane,250).value_or(0)!=1)continue;
                for(auto site:sites){
                    const auto address=word(lane,site[0]),resolved=word(lane,site[1]),loaded=word(lane,site[2]);
                    if(!address||!resolved)continue;
                    nlohmann::json row={{"lane",lane},{"address_site",site[0]},
                        {"guest",*address},{"resolved_nonzero",*resolved!=0},{"load_executed",loaded.has_value()}};
                    if(loaded)row["loaded"]=*loaded;
                    uint32_t backing=0;
                    if(*address && LibKernel::Memory::TryReadBacking(*address,&backing,4)){
                        row["backing"]=backing;
                        row["gpu_dirty"]=context.GetBufferCache().HasGpuDirtyBytes(*address,4);
                        row["cpu_modified"]=context.GetBufferCache().IsRegionCpuModified(*address,4);
                    }
                    bda.push_back(std::move(row));
                }
            }
            (*report)["bda_reads"]=std::move(bda);
            // Guest ISA gate: lane<count, list DWORD0 at base+lane,
            // object flag at id*220+208, then (flags&8)==0. Compare
            // independently captured descriptor words with actual execution
            // of the same lane in this dispatch, not another scene/frame.
            if(report->contains("first_gate")) {
                auto& gate=(*report)["first_gate"];
                auto rows=nlohmann::json::array();
                uint32_t compared=0,mismatched=0,unavailable=0;
                if(gate.contains("lanes")) for(const auto& row:gate["lanes"]) {
                    const auto lane=row["lane"].get<uint32_t>();
                    if(lane>=P::Lanes || !row.contains("flags")) {++unavailable;continue;}
                    const auto lane_id=word(lane,187),count=word(lane,208),active=word(lane,210),
                        object=word(lane,230),flags=word(lane,246),pass=word(lane,250);
                    if(!lane_id||!count||!active||!object||!flags||!pass||*lane_id!=lane) {++unavailable;continue;}
                    const auto expected_count=gate["count"].get<uint32_t>();
                    const auto expected_object=row["object_id"].get<uint32_t>();
                    const auto expected_flags=row["flags"].get<uint32_t>();
                    const bool expected_pass=(lane<expected_count)&&((expected_flags&8u)==0u);
                    const bool same=*count==expected_count && *active==1u && *object==expected_object &&
                        *flags==expected_flags && *pass==uint64_t(expected_pass);
                    ++compared;mismatched+=!same;
                    rows.push_back({{"lane",lane},{"expected_count",expected_count},{"actual_count",*count},
                        {"expected_object",expected_object},{"actual_object",*object},
                        {"expected_flags",expected_flags},{"actual_flags",*flags},
                        {"expected_pass",expected_pass},{"actual_pass",*pass},{"actual_active",*active},{"match",same}});
                }
                gate["execution_replay"]={{"status",compared?(mismatched?"divergence":"match"):"unavailable"},
                    {"compared",compared},{"mismatched",mismatched},{"unavailable",unavailable},{"rows",std::move(rows)}};
            }
            (*report)["exec_trace"]={{"fingerprint",fmt::format("{:016x}",bindings.runtime->program->ds_light_math_variant)},
                {"site_ids",P::Sites},{"lanes",P::Lanes},{"words_per_site",P::WordsPerSite},
                {"words",std::move(words)}};
        }else (*report)["exec_trace_status"]="readback_failed";
    } else if(bindings.runtime->program->shader_hash==P::Shader) {
        (*report)["exec_trace_status"]="no_validated_variant";
    }
    namespace A=DemonsCountArithmeticProbe;
    if(bindings.runtime->program->shader_hash==A::Shader) {
        const auto fp=bindings.runtime->program->ds_light_math_variant;
        const auto& native=bindings.shared_memory;
        std::array<uint32_t,A::WordCount> raw{};
        auto evidence=nlohmann::json{{"status","no_validated_variant"},{"fingerprint",fmt::format("{:016x}",fp)}};
        if(A::Supported(fp)){
            evidence["status"]="readback_failed";
            if(native.buffer&&native.range==raw.size()*sizeof(uint32_t)&&ReadDsGpuWords(context,native.buffer,native.offset,raw)){
                evidence["raw"]=raw;
                const auto get=[&](A::Site site)->std::optional<uint32_t>{
                    const auto i=1+2*static_cast<uint32_t>(site);
                    return raw[i]?std::optional<uint32_t>(raw[i+1]):std::nullopt;
                };
                constexpr std::array<const char*,A::SiteCount> names{"secondary_limit","secondary_index","secondary_length","secondary_resolved_offset","secondary_in_range","secondary_load","secondary_value",
                    "primary_index","primary_length","primary_resolved_offset","primary_in_range","primary_load","primary_limit","primary_value","count","groups",
                    "store_groups","store_y","store_count","store_secondary","store_z"};
                auto sites=nlohmann::json::object();
                for(uint32_t i=0;i<A::SiteCount;++i)if(auto v=get(static_cast<A::Site>(i)))sites[names[i]]=*v;
                evidence["sites"]=std::move(sites);
                const auto sec_limit=get(A::SecLimit),sec_load=get(A::SecLoad),sec=get(A::SecValue),
                    pri=get(A::PriLoad),pri_limit=get(A::PriLimit),count=get(A::Count),groups_site=get(A::Groups);
                if(sec_limit&&sec&&pri&&pri_limit&&count&&groups_site&&(!*sec_limit||sec_load||fp==A::Two)){
                    const auto expected=DemonsCausalMatrix::CountExpected(*pri,sec_load.value_or(0),*pri_limit,*sec_limit);
                    const std::array<std::optional<uint32_t>,5> stores{get(A::StoreGroup),get(A::StoreZ),get(A::StoreY),get(A::StoreCount),get(A::StoreSecondary)};
                    bool complete=true,match=*sec==expected[4]&&*count==expected[3]&&*groups_site==expected[0];
                    for(size_t i=0;i<stores.size();++i){complete&=stores[i].has_value();if(stores[i])match&=*stores[i]==expected[i];}
                    evidence["actual_operand_expected"]=expected;
                    evidence["status"]=complete?(match?"match":"arithmetic_or_store_divergence"):"store_not_executed";
                    if(complete){std::array<uint32_t,5> out{};for(size_t i=0;i<5;++i)out[i]=*stores[i];evidence["stores"]=out;}
                }else evidence["status"]="incomplete_operands";
            }
        }
        (*report)["actual_execution"]=std::move(evidence);
    }
    (*report)["groups_valid"]=!indirect || ReadDsGpuWords(context,indirect,indirect_offset,groups);
    (*report)["groups"]=groups;
    auto after=nlohmann::json::array();
    for (const auto& res:bindings.runtime->program->info.buffers) {
        const auto slot=res.descriptor_index;
        if (!res.written || slot>=bindings.buffer_sources.size()) continue;
        const auto& src=bindings.buffer_sources[slot];
        if (!src.size || src.size%4) continue;
        if (src.size>131072) {
            const auto watch=(*report)["watch_address"].get<uint64_t>();
            if (watch && watch>=src.address && watch-src.address<=src.size &&
                20<=src.size-(watch-src.address) && (watch-src.address)%4==0)
                after.push_back({{"slot",slot},{"guest",src.address},{"watched_address",watch},
                    {"watched_words",ReadDsCountSlice(context,bindings,slot,(watch-src.address)/4,5)}});
            continue;
        }
        auto words=ReadDsBoundWords(context,bindings,slot,src.size/4);
        after.push_back({{"slot",slot},{"guest",src.address},
            {"status",words.size()==src.size/4?"captured":"incomplete_gpu_read"},
            {"gpu_words",std::move(words)}});
    }
    (*report)["after"]=std::move(after);
    // Independent unsigned arithmetic oracle, read-only on captured data.
    if(DemonsCausalMatrix::Evidence() && bindings.runtime->program->shader_hash==0xcb3b9c1ea64fe876ull) {
        auto result=nlohmann::json{{"status","unavailable"}};
        const auto& s=(*report)["srt"];
        if(s.size()==18) {
            const auto u=[&](size_t i){return s[i].get<uint32_t>();};
            const auto address=[&](size_t i){return uint64_t{u(i)}|(uint64_t{u(i+1)&0xffffu}<<32u);};
            const auto words=[&](const char* side,uint64_t addr)->const nlohmann::json* {
                const nlohmann::json* found=nullptr;
                for(const auto& x:(*report)[side]) if(x.value("guest",uint64_t{0})==addr && x.contains("gpu_words")) {
                    if(found)return nullptr;
                    found=&x["gpu_words"];
                }
                return found;
            };
            const auto* primary=words("before",address(6));
            const auto* secondary=words("before",address(2));
            const auto* output=words("after",address(10));
            if(primary && output && output->size()>=5) {
                const auto flat=s.get<std::vector<uint32_t>>();
                const auto p=primary->get<std::vector<uint32_t>>();
                const auto sec=secondary?secondary->get<std::vector<uint32_t>>():std::vector<uint32_t>{};
                if(const auto expected=DemonsCausalMatrix::ReplayCount(flat,p,sec)){
                    std::array<uint32_t,5> actual{};for(size_t i=0;i<5;++i)actual[i]=(*output)[i].get<uint32_t>();
                    result={{"status",*expected==actual?"match":"divergence"},{"expected",*expected},{"actual",actual},
                        {"oracle_version",2},{"secondary_limit",flat[DemonsCausalMatrix::SecondaryLimit]},
                        {"secondary_index",flat[DemonsCausalMatrix::SecondaryIndex]}};
                }
            }
        }
        (*report)["independent_replay"]=std::move(result);
        auto& actual=(*report)["actual_execution"];
        const auto& independent=(*report)["independent_replay"];
        if(actual.contains("stores")&&independent.contains("actual")) {
            actual["stores_match_gpu_readback"]=actual["stores"]==independent["actual"];
            actual["actual_operands_match_snapshot_oracle"]=actual["actual_operand_expected"]==independent["expected"];
        }
        Log::WriteToConsoleAndLog(fmt::format("[DS_COUNT_ARITH_RESULT] frame={} submit={} snapshot={} executed={}\n",
            (*report)["frame"].get<int32_t>(),(*report)["submit"].get<uint64_t>(),independent.value("status",std::string("unavailable")),actual.value("status",std::string("unavailable"))));
    }
    (*report)["capture_tick_after"]=context.GetCommandScheduler().CurrentTick();
    const auto folder=Config::GetShaderDumpFolder()/"queue-state";
    std::error_code ec;
    std::filesystem::create_directories(folder,ec);
    if (ec) return;
    const auto name=fmt::format("ds_count_producer_{}{}_f{}_s{}_n{}_p{}.json",
        (*report)["shader"].get<std::string>(),(*report).value("secondary_active",false)?"_secondary":"",(*report)["frame"].get<int32_t>(),
        (*report)["submit"].get<uint64_t>(),(*report)["capture_seq"].get<uint32_t>(),(*report)["phase"].get<uint32_t>());
    std::ofstream file(folder/name,std::ios::trunc);
    if (file) file << *report;
    file.close();
    Log::WriteToConsoleAndLog(fmt::format("[DS_COUNT_PRODUCER] file={} saved={}\n",name,file.good()));
}

// Read only the five real draw records at an overlapping compute writer,
// not its entire allocation. Capture actual post-dispatch values in three
// scene phases, with a strict session budget; the binding's write range is
// only a candidate until the words and the consumer's feedback agree.
static void CaptureDsGeometryWriter(RenderContext& context,
                                    const PreparedBindings& bindings, uint64_t submit,
                                    std::array<uint32_t,3> groups,
                                    vk::Buffer indirect = {}, uint64_t indirect_offset = 0) {
    namespace D = DemonsDominantGeometry;
    if (!DsTraceEnabled() || Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
    static uint32_t files = 0, upstream_files = 0;
    if (files >= 12 && upstream_files >= 12) return;
    const auto frame = context.GetGpu().GetFrameNum();
    const auto& gate = DemonsAdaptiveCapture::Current();
    const uint32_t phase = gate.PostTransitionDarkDue(frame) ? 3u :
        gate.DarkOnsetDue(frame) ? 2u : gate.BrightSceneActive(frame) ? 1u : 0u;
    const auto& program = *bindings.runtime->program;
    const auto& snapshot = *bindings.runtime->resources;
    static std::unordered_map<uint64_t,uint32_t> captured;
    if ((captured[program.shader_hash] & (1u<<phase)) != 0) return;
    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json upstream = nlohmann::json::array();
    for (size_t i=0; i<program.info.buffers.size(); ++i) {
        const auto& resource = program.info.buffers[i];
        const auto slot = resource.descriptor_index;
        if (!resource.written || slot >= bindings.buffer_sources.size() ||
            slot >= bindings.buffers.size()) continue;
        const auto& source = bindings.buffer_sources[slot];
        const bool output_overlap = D::CountOverlap(source.address,source.size);
        const bool input_overlap = D::CountInputOverlap(source.address,source.size);
        if (!output_overlap && !input_overlap) continue;
        const auto& native = bindings.buffers[slot];
        const auto offset_word = program.bindings.memory_offset_dword + slot/4u;
        if (!native.buffer || native.range == VK_WHOLE_SIZE ||
            offset_word >= bindings.shader_data.size()) continue;
        const uint64_t adjustment =
            (bindings.shader_data[offset_word] >> ((slot%4u)*8u)) & 255u;
        if (input_overlap && upstream_files<12) {
            const auto begin=std::max(source.address,D::CountInputAddress);
            const auto end=std::min(source.address+source.size,D::CountInputAddress+uint64_t{D::CountInputWords}*4u);
            const auto offset=begin-source.address;
            if (begin%4==0 && end>begin && (end-begin)%4==0 &&
                adjustment<=native.range && offset<=native.range-adjustment &&
                end-begin<=native.range-adjustment-offset) {
                std::vector<uint32_t> words((end-begin)/4);
                if (ReadDsGpuWords(context,native.buffer,native.offset+adjustment+offset,words))
                    upstream.push_back({{"slot",slot},{"guest_base",source.address},
                        {"guest_size",source.size},{"begin",begin},{"words",words},
                        {"dynamic_write",resource.written_dynamic},
                        {"static_write_begin",resource.written_begin},{"static_write_end",resource.written_end}});
            }
        }
        if (!output_overlap || files>=12) continue;
        for (auto site: D::IndirectRecords) {
            if (site < source.address) continue;
            const uint64_t offset = site-source.address;
            constexpr uint64_t bytes = 5*sizeof(uint32_t);
            if (offset > source.size || bytes > source.size-offset ||
                adjustment > native.range || offset > native.range-adjustment ||
                bytes > native.range-adjustment-offset) continue;
            std::array<uint32_t,5> raw {}, backing {};
            const bool backing_ok = LibKernel::Memory::TryReadBacking(site,backing.data(),bytes);
            if (!ReadDsGpuWords(context,native.buffer,native.offset+adjustment+offset,raw)) continue;
            rows.push_back({{"site",site},{"slot",slot},{"guest_base",source.address},
                {"guest_size",source.size},{"record",raw},{"backing_ok",backing_ok},
                {"backing",backing},{"instance_count",raw[1]},
                {"static_write_begin",resource.written_begin},{"static_write_end",resource.written_end},
                {"dynamic_write",resource.written_dynamic}});
        }
    }
    if (rows.empty() && upstream.empty()) return;
    if (indirect) (void)ReadDsGpuWords(context,indirect,indirect_offset,groups);
    // Capture real, disjoint input/output descriptor views; do not infer counts.
    nlohmann::json count_source = {{"status","not_draw_builder"}};
    if (program.shader_hash == 0xf0c140ea8df8f532ull) {
        count_source["status"] = "unexpected_layout";
        if (snapshot.buffers.size()==4 && bindings.buffer_sources.size()>=4 &&
            bindings.buffers.size()>=4 && snapshot.flattened_srt.size()==19 &&
            groups[0]==1 && groups[1]==1 && groups[2]==1) {
            const auto& src = bindings.buffer_sources[D::CountInputSlot];
            const auto& dst = bindings.buffer_sources[3];
            const auto sdesc = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[D::CountInputSlot]);
            const auto ddesc = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[3]);
            bool source_read_only = false;
            bool alias = false;
            for (const auto& res : program.info.buffers) {
                if (res.descriptor_index == D::CountInputSlot) source_read_only = !res.written;
                if (res.written && res.descriptor_index < bindings.buffer_sources.size()) {
                    const auto& w = bindings.buffer_sources[res.descriptor_index];
                    alias |= D::Overlaps(src.address, src.size, w.address, w.size);
                }
            }
            if (source_read_only && !alias && sdesc.Stride()==4 && ddesc.Stride()==4 &&
                src.address==D::CountInputAddress && src.size==D::CountInputWords*4u && dst.size && dst.size<=131072 &&
                dst.address==D::IndirectRecords[0]) {
                const auto input = ReadDsBoundWords(context,bindings,D::CountInputSlot,src.size/4);
                const auto output = ReadDsBoundWords(context,bindings,3,dst.size/4);
                count_source = {{"status",input.size()==src.size/4 && output.size()==dst.size/4 ?
                    "captured" : "incomplete_gpu_read"},
                    {"guest",src.address},{"bytes",src.size},{"output_guest",dst.address},
                    {"output_bytes",dst.size},{"active_threads",snapshot.flattened_srt[18]},
                    {"source_slot",D::CountInputSlot},{"output_slot",D::CountOutputSlot},
                    {"gpu_words",input},{"output_words",output},
                    {"gpu_dirty",context.GetBufferCache().HasGpuDirtyBytes(src.address,src.size)}};
                std::vector<uint32_t> backing(src.size/4);
                const bool backing_ok = LibKernel::Memory::TryReadBacking(src.address,backing.data(),src.size);
                count_source["backing_ok"] = backing_ok;
                if (backing_ok) count_source["backing_words"] = backing;
                auto pairs = nlohmann::json::array();
                const auto active = std::min<uint32_t>(snapshot.flattened_srt[18],64u);
                for (uint32_t lane=0; lane<active; ++lane) for (uint32_t draw=0; draw<21; ++draw) {
                    const auto si=D::CountSourceIndex(lane,draw), di=D::CountDestIndex(lane,draw);
                    if (si>=input.size() || di>=output.size()) continue;
                    const auto address=src.address+4ull*si;
                    const auto stamp=context.GetBufferCache().LastGeometryInputWrite(address);
                    nlohmann::json row={{"lane",lane},{"draw",draw},{"input_index",si},
                        {"output_index",di},{"source",input[si]},{"output",output[di]},
                        {"equal",input[si]==output[di]},{"source_address",address}};
                    if (stamp.path) row["last_write"]={{"path",stamp.path},{"tick",stamp.tick},
                        {"source",stamp.source},{"shader",fmt::format("{:016x}",stamp.shader)},
                        {"begin",stamp.begin},{"end",stamp.end}};
                    if (const auto* w=FindDsBufferProducer(frame,address,4,false))
                        row["compute_candidate"]={{"shader",fmt::format("{:016x}",w->shader)},
                            {"frame",w->frame},{"submit",w->submit},{"address",w->address},
                            {"bytes",w->size},{"fill",w->fill}};
                    if (backing_ok) row["backing"]=backing[si];
                    pairs.push_back(std::move(row));
                }
                count_source["pairs"]=std::move(pairs);
            } else count_source["status"] = "source_not_proven_read_only_or_layout_mismatch";
        }
    }
    nlohmann::json descriptors = nlohmann::json::array();
    for (size_t i=0; i<snapshot.buffers.size(); ++i)
        descriptors.push_back({{"index",i},{"words",snapshot.buffers[i].dwords}});
    nlohmann::json images = nlohmann::json::array();
    for (const auto& desc: snapshot.images) images.push_back(desc.dwords);
    nlohmann::json samplers = nlohmann::json::array();
    for (const auto& desc: snapshot.samplers) samplers.push_back(desc.dwords);
    const auto folder = Config::GetShaderDumpFolder() / "queue-state";
    std::error_code ec;
    std::filesystem::create_directories(folder,ec);
    if (ec) return;
    const auto name = fmt::format("ds_geometry_writer_{:016x}_f{}_s{}_p{}.json",
        program.shader_hash,frame,submit,phase);
    std::ofstream file(folder/name,std::ios::trunc);
    if (!file) return;
    file << nlohmann::json{{"frame",frame},{"submit",submit},{"phase",phase},
        {"shader",fmt::format("{:016x}",program.shader_hash)},{"groups",groups},
        {"indirect",bool(indirect)},{"user_data",snapshot.user_data},{"srt",snapshot.flattened_srt},
        {"shader_data",bindings.shader_data},{"buffers",descriptors},{"images",images},
        {"samplers",samplers},{"records",rows},{"count_source",count_source},{"source_writes",upstream}};
    file.close();
    if (file.good()) {
        if (!rows.empty()) ++files;
        if (!upstream.empty()) ++upstream_files;
        captured[program.shader_hash] |= 1u<<phase;
        Log::WriteToConsoleAndLog(fmt::format(
            "[DS_GEOMETRY_WRITER] frame={} shader=0x{:016x} records={} file={}\n",
            frame,program.shader_hash,rows.size(),name));
    }
}

// Demon's Souls ray-hit provenance: the 767 capture happened at frame 206,
// BEFORE the gameplay lighting went dark. Track all producer dispatch activity
// and capture only during the observed onset/post-transition phases. Pair with
// the relight consumer's activity stamp and ray_hits_f*.bin to determine whether
// a missing ray value was written by the producer or changed later. Bounded
// readbacks only: at most one onset and three dark 8-KiB snapshots per session.
static void CaptureDsRayHitProducer(RenderContext& context,
                                    const PreparedBindings& bindings,
                                    uint64_t submit, std::array<uint32_t, 3> groups,
                                    bool groups_valid) {
	const auto& program = *bindings.runtime->program;
	if (!DemonsPcDppPolicy::CaptureProducer(DsTraceEnabled(), program.shader_hash) ||
	    Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	const auto frame = context.GetGpu().GetFrameNum();
	const auto& gate = DemonsAdaptiveCapture::Current();
	const bool onset = gate.DarkOnsetDue(frame);
	const bool late = gate.PostTransitionDarkDue(frame);
	const bool valid_dispatch = groups_valid && groups[0] && groups[1] && groups[2];
	++ds_ray_producer_activity.observed;
	if (valid_dispatch) {
		++ds_ray_producer_activity.valid;
		ds_ray_producer_activity.last_frame = frame;
		ds_ray_producer_activity.last_submit = submit;
		ds_ray_producer_activity.last_tick = context.GetCommandScheduler().CurrentTick();
	}
	static DemonsAdaptiveCapture::RayProducerCaptureGate capture_gate;
	capture_gate.ObserveFrame(frame);
	static uint32_t reports = 0;
	const bool followup = !late && capture_gate.FollowupDue(frame);
	const bool due = capture_gate.Due(frame, onset, late) || followup;
	// 231552: the consumed dark probes were not in the previous fixed list.
	// Scan bounded active maps even after the initial generic capture budget.
	static uint32_t targeted_reports = 0;
	const bool ordinary = DemonsPcDppPolicy::CaptureActive(ds_ray_producer_activity.valid, due);
	if (!valid_dispatch) return;
	if(ds_ray_producer_activity.valid>128){ds_ray_source_history.Invalidate();return;}
	const auto note = [&](const char* status, uint32_t slot, uint64_t address) {
		if (reports++ >= 32) return;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_RAY_PRODUCER] frame={} submit={} status={} slot={} guest=0x{:x} "
		    "groups_valid={} x={} y={} z={} bvh={} has_address_writes={}\n",
		    context.GetGpu().GetFrameNum(), submit, status, slot, address,
		    groups_valid, groups[0], groups[1], groups[2],
		    program.info.bvh, program.has_address_writes));
	};
	if (!groups_valid || !groups[0] || !groups[1] || !groups[2]) {
        if(!groups_valid)ds_ray_source_history.Invalidate();
		note("inactive_dispatch", 0, 0);
		return;
	}
	constexpr uint64_t expected_bytes = 188743680ull;
	// Exact atlas IDs from DS_LIGHT_TERM at frames 481/721/961 in 231552.
	constexpr std::array<uint32_t,3> consumed {4701u,24540u,23322u};
	std::vector<uint32_t> probes(consumed.begin(), consumed.end());
	probes.push_back(4445u); // historical control
	for (auto p : ds_ray_producer_targets.probes)
		if (std::find(probes.begin(), probes.end(), p) == probes.end()) probes.push_back(p);
	for (auto p : ds_relight_trace_targets)
		if (p != UINT32_MAX && std::find(probes.begin(), probes.end(), p) == probes.end()) probes.push_back(p);
	const auto target_candidates = probes;
	bool targeted = false;
	// The captured guest/SPIR-V is a scatter copy, not a ray tracer.
	// Read its actual source row index and payload, not an unrelated output
	// probe that the current dispatch may never touch.
	using namespace DemonsRayCopyPolicy;
	const auto& snapshot = *bindings.runtime->resources;
	std::vector<uint32_t> map;
	uint32_t map_base = 0, active_rows = 0;
	nlohmann::json source_report {{"status", "unexpected_copy_layout"}};
	if (snapshot.buffers.size() == 3 && snapshot.flattened_srt.size() == 16 &&
	    program.info.buffers.size() == 3 && bindings.buffer_sources.size() >= 3 &&
	    bindings.buffers.size() >= 3 && groups[1] == 1 && groups[2] == 1 &&
	    !bindings.shader_data.empty() && bindings.shader_data[0] == 0) {
		const auto mapping = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[0]);
		const auto payload = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[1]);
		const auto target = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[2]);
		if (mapping.Stride() == 4 && mapping.NumRecords() <= 65536 &&
		    payload.Stride() == 8 && target.Stride() == 8 &&
		    target.NumRecords() == ProbeCount * RaysPerProbe &&
		    program.info.buffers[0].descriptor_index == 0 &&
		    program.info.buffers[1].descriptor_index == 1 &&
		    program.info.buffers[2].descriptor_index == 2 &&
		    !program.info.buffers[0].written && !program.info.buffers[1].written) {
			map_base = snapshot.flattened_srt[15];
			active_rows = ActiveRows(map_base, snapshot.flattened_srt[14], mapping.NumRecords(), groups[0]);
			map = ReadDsBoundWords(context, bindings, 0, mapping.NumRecords());
			source_report = {{"status", map.size() == mapping.NumRecords() ? "mapped" : "map_read_failed"},
			    {"base", map_base}, {"active_rows", active_rows}, {"map_records", map.size()},
			    {"map_guest", bindings.buffer_sources[0].address}, {"payload_guest", bindings.buffer_sources[1].address},
			    {"payload_bytes", bindings.buffer_sources[1].size}};
			if (map.size() != mapping.NumRecords()) map.clear();
			// The captured SPIR-V (IDs 93/94, 104/149) explicitly tests
			// 0xffffff < raw, then forms the wrapped 32-bit byte address.
			// Retain the actual map to make later CPU replays auditable.
			if (!map.empty()) {
				uint32_t flagged = 0, destinations = 0;
				const auto end = std::min<uint64_t>(map.size(), uint64_t{map_base} + active_rows);
				for (uint32_t i = map_base; i < end; ++i) {
					flagged += map[i] > 0xffffffu;
					const auto id = Destination(map[i]);
					if (id == NoRow) continue;
					++destinations;
					if (probes.size() < 6 && std::find(probes.begin(), probes.end(), id) == probes.end())
						probes.push_back(id);
				}
				for (uint64_t i = end; i > map_base && probes.size() < 8;) {
					const auto id = Destination(map[--i]);
					if (id != NoRow && std::find(probes.begin(), probes.end(), id) == probes.end())
						probes.push_back(id);
				}
				for (auto p : target_candidates)
					targeted |= Find(map, map_base, active_rows, p).matches != 0;
				source_report["map_words"] = map;
				source_report["flagged_rows"] = flagged;
				source_report["valid_destination_rows"] = destinations;
				source_report["decode"] = "raw_gt_ffffff_wrapped_u32_byte_offset";
			}
		}
	}
	if(DsExtendedRayHistoryEnabled()&&LibKernel::DemonsFileReadHistory::Enabled()) RememberDsRaySourceRows(context,bindings,submit,map,map_base,active_rows);
	if (!ordinary && (!targeted || targeted_reports>=20)) return;
	constexpr uint64_t words_per_probe = 256ull * 2ull;
	constexpr uint64_t bytes_per_probe = words_per_probe * sizeof(uint32_t);
	for (const auto& resource: program.info.buffers) {
		const auto slot = resource.descriptor_index;
		if (!resource.written || slot >= bindings.buffer_sources.size() ||
		    slot >= bindings.buffers.size()) continue;
		const auto& source = bindings.buffer_sources[slot];
		const auto& native = bindings.buffers[slot];
		if (!source.address || source.size < expected_bytes ||
		    !native.buffer || native.range == VK_WHOLE_SIZE) continue;
		const uint64_t last_byte = (uint64_t{*std::max_element(probes.begin(), probes.end())} +
		    1ull) * bytes_per_probe;
		const auto offset_word = program.bindings.memory_offset_dword + slot / 4u;
		const bool adjustment_known = offset_word < bindings.shader_data.size();
		// Non-push-data shaders use zero offset. Do not reject a valid
		// resource merely because there is no packed adjustment word.
		const uint64_t adjustment = adjustment_known
		    ? (bindings.shader_data[offset_word] >> ((slot % 4u) * 8u)) & 255u
		    : 0ull;
		if (adjustment > native.range || last_byte > native.range - adjustment ||
		    last_byte > source.size) {
			note("out_of_range", slot, source.address);
			continue;
		}
		std::vector<uint32_t> words(probes.size() * words_per_probe);
		std::vector<vk::BufferCopy> copies(probes.size());
		for (size_t i = 0; i < probes.size(); ++i) {
			copies[i] = {native.offset + adjustment + uint64_t{probes[i]} * bytes_per_probe,
			             i * bytes_per_probe, bytes_per_probe};
		}
		if (!ReadDsGpuWordRanges(context, native.buffer, copies, words)) {
			note("readback_failed", slot, source.address);
			return;
		}
		nlohmann::json entries = nlohmann::json::array();
		for (size_t i = 0; i < probes.size(); ++i) {
			const auto row = std::span{words}.subspan(i * words_per_probe, words_per_probe);
			std::array<uint32_t, 2> colored_by_half {};
			std::array<uint32_t, 2> valid_depth_by_half {};
			uint64_t digest = 14695981039346656037ull;
			for (uint32_t j = 0; j < 256; ++j) {
				const auto color = row[j * 2u];
				const auto data = row[j * 2u + 1u];
				colored_by_half[j / 128u] += (color & 0x00ffffffu) != 0u;
				const uint32_t depth = (data >> 16u) & 0x7fffu;
				valid_depth_by_half[j / 128u] += depth != 0 && depth < 0x7c00u;
				digest = (digest ^ ((uint64_t{data} << 32u) | color)) *
				         1099511628211ull;
			}
			nlohmann::json entry {{"probe", probes[i]},
			    {"guest", source.address + uint64_t{probes[i]} * bytes_per_probe},
			    {"colored_first_second_128", colored_by_half},
			    {"finite_depth_first_second_128", valid_depth_by_half},
			    {"digest", fmt::format("{:016x}", digest)},
			    {"ray_words", std::vector<uint32_t>(row.begin(), row.end())}};
			if (!map.empty()) {
				const auto match = DemonsRayCopyPolicy::Find(map, map_base, active_rows, probes[i]);
				entry["mapping_matches"] = match.matches;
				if (match.matches == 1) {
					const auto& input = bindings.buffer_sources[1];
					const auto& view = bindings.buffers[1];
					const auto off_word = program.bindings.memory_offset_dword;
					const uint64_t off = off_word < bindings.shader_data.size() ?
					    (bindings.shader_data[off_word] >> 8u) & 255u : 0u;
					const uint64_t byte = uint64_t{match.row} * bytes_per_probe;
					entry["source_row"] = match.row;
					entry["raw_map_word"] = map[match.row];
					entry["source_guest"] = input.address + byte;
					if (view.buffer && view.range != VK_WHOLE_SIZE &&
					    byte <= input.size && bytes_per_probe <= input.size - byte &&
					    off <= view.range && byte <= view.range - off &&
					    bytes_per_probe <= view.range - off - byte) {
						std::vector<uint32_t> input_words(words_per_probe);
						if (ReadDsGpuWords(context, view.buffer, view.offset + off + byte, input_words)) {
							uint32_t different = 0, colored = 0;
							for (size_t j = 0; j < words_per_probe; j += 2) {
								different += input_words[j] != row[j] || input_words[j+1] != row[j+1];
								colored += (input_words[j] & 0xffffffu) != 0;
							}
							entry["source_different_pairs"] = different;
							entry["source_colored"] = colored;
							entry["source_words"] = std::move(input_words);
						} else entry["source_status"] = "read_failed";
					} else entry["source_status"] = "out_of_range";
					const auto addr = input.address + byte;
					const auto* writer = FindDsBufferProducer(frame, addr, bytes_per_probe, false);
					if (writer) entry["source_overlapping_writer"] = {
					    {"shader", fmt::format("{:016x}", writer->shader)}, {"frame", writer->frame},
					    {"submit", writer->submit}, {"address", writer->address}, {"bytes", writer->size},
					    {"fill", writer->fill}};
					const auto transfers = context.GetBufferCache().RecentProbeRayTransfers(addr, bytes_per_probe);
					auto provenance = nlohmann::json::array();
					for (const auto& w : transfers) if(w.path) provenance.push_back({
					    {"path",w.path},{"tick",w.tick},{"source",w.source},{"shader",fmt::format("{:016x}",w.shader)},
					    {"begin",w.begin},{"end",w.end}});
					entry["source_transfer_history"] = std::move(provenance);
					entry["source_gpu_dirty"] = context.GetBufferCache().HasGpuDirtyBytes(addr, bytes_per_probe);
					entry["source_cpu_modified"] = context.GetBufferCache().IsRegionCpuModified(addr, bytes_per_probe);
					std::array<uint32_t, words_per_probe> backing {};
					if (LibKernel::Memory::TryReadBacking(addr, backing.data(), bytes_per_probe)) {
						uint32_t different = 0, colored = 0;
						for (size_t j = 0; j < words_per_probe; j += 2) {
							different += backing[j] != row[j] || backing[j+1] != row[j+1];
							colored += (backing[j] & 0xffffffu) != 0;
						}
						entry["backing_different_pairs"] = different;
						entry["backing_colored"] = colored;
						entry["source_backing_words"] = backing;
						if (entry.contains("source_words")) {
							uint32_t differences = 0;
							const auto& gpu = entry["source_words"];
							for (size_t j = 0; j < words_per_probe; j += 2)
								differences += gpu[j].get<uint32_t>() != backing[j] ||
								               gpu[j+1].get<uint32_t>() != backing[j+1];
							entry["source_vs_backing_different_pairs"] = differences;
						}
					}
				}
			}
			entries.push_back(std::move(entry));
		}
		const auto folder = Config::GetShaderDumpFolder() / "queue-state";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		if (ec) {
			note("create_directory_failed", slot, source.address);
			return;
		}
		const auto name = fmt::format("ds_ray_producer_f{}_s{}_t{}{}.json", frame, submit,
		                             context.GetCommandScheduler().CurrentTick(), targeted ? "_target" : "");
		nlohmann::json output {{"frame", frame}, {"submit", submit},
		    {"shader", "684e797dce67bb8e"},
		    {"groups", groups}, {"bvh", program.info.bvh},
		    {"has_address_writes", program.has_address_writes},
		    {"output_slot", slot}, {"output_guest", source.address},
		    {"output_bytes", source.size},
		    {"source_adjustment", adjustment},
		    {"source_adjustment_known", adjustment_known},
		    {"phase", late ? "post_transition" : followup ? "producer_followup" : onset ? "dark_onset" : "active_producer"},
		    {"selected_from_consumer_frame", ds_ray_producer_targets.consumer_frame},
		    {"producer_tick", context.GetCommandScheduler().CurrentTick()},
		    {"producer_dispatches_seen", ds_ray_producer_activity.valid},
		    {"consumed_probe_ids", consumed}, {"relight_trace_targets",ds_relight_trace_targets},
		    {"targeted_probe_ids",target_candidates}, {"targeted_map_match", targeted},
		    {"entries", std::move(entries)}};
		// Save exact live shader inputs on each bounded active producer dispatch.
		output["copy_source"] = std::move(source_report);
		output["srt"] = snapshot.flattened_srt;
		output["shader_data"] = bindings.shader_data;
		output["buffer_descriptors"] = nlohmann::json::array();
		for (const auto& desc : snapshot.buffers) output["buffer_descriptors"].push_back(desc.dwords);
		output["image_descriptors"] = nlohmann::json::array();
		for (const auto& desc : snapshot.images) output["image_descriptors"].push_back(desc.dwords);
		output["sampler_descriptors"] = nlohmann::json::array();
		for (const auto& desc : snapshot.samplers) output["sampler_descriptors"].push_back(desc.dwords);
		std::ofstream file(folder / name, std::ios::trunc);
		if (!file) {
			note("open_failed", slot, source.address);
			return;
		}
		file << output;
		file.close();
		if (!file.good()) {
			note("write_failed", slot, source.address);
			return;
		}
		if (targeted) ++targeted_reports;
		if (followup) capture_gate.NoteFollowup(frame);
		else capture_gate.NoteCaptured(frame, onset, late);
		note(late ? "captured_post_transition" :
		     followup ? "captured_producer_followup" : onset ? "captured_onset" : "captured_active", slot, source.address);
		Log::WriteToConsoleAndLog(fmt::format("[DS_RAY_PRODUCER_FILE] frame={} submit={} file={}\n", frame, submit, name));
		return;
	}
	note("no_matching_written_ray_buffer", 0, 0);
}

// Scene HDR and its deferred G-buffer sampled at precisely the SAME
// normalized 8x8 coordinates, immediately after queue 33 executes.
// Capture the early fade separately from the post-transition dark candidate.
// Exactly one bridge per phase; it pairs G-buffer and HDR at the same
// positions, and never changes a draw, dispatch, or shader.
static void CaptureDsLightBridge(RenderContext& context, const PreparedBindings& bindings,
                                 uint64_t submit, const std::array<uint32_t, 3>& groups,
                                 bool groups_valid, bool before=false) {
	using namespace DemonsQueueProbe;
    const auto shader=bindings.runtime->program->shader_hash;
    const uint32_t queue=DemonsCausalMatrix::LightingQueue(shader);
    if(queue==UINT32_MAX)return;
    const size_t queue_slot=queue==0?0u:queue==8?1u:2u;
    const bool root=DemonsPrelightTrace::Enabled();
	if (!DsTraceEnabled() ||
	    (!root && !before && (!groups_valid || groups[0] == 0 || groups[1] == 0 || groups[2] == 0)) ||
	    Config::GetShaderDumpFolder().filename() != "ds-lighting-dumps") return;
	const int32_t frame = context.GetGpu().GetFrameNum();
	const auto& gate = DemonsAdaptiveCapture::Current();
	// The HDR producer queue may run BEFORE the coefficient/atlas pair
	// is marked captured in this guest frame. Sampling its output must
	// not depend on NoteCaptured() having run first.
    const bool p8_window=root&&DemonsPrelightTrace::P8Frame(frame);
    if(root&&DemonsPrelightTrace::Phase(frame)<0&&!p8_window)return;
	const char* phase = root?"root":gate.PostTransitionDarkDue(frame)
	    ? "post_transition" : gate.DarkOnsetDue(frame) ? "onset"
	    : gate.BrightSceneActive(frame) ? "bright"
	    : gate.CapturedFrame(frame) ? "paired_dark" : nullptr;
	if (!phase) return;
    struct BridgeGate{bool bright=false,onset=false,dark=false;uint32_t post=0;int32_t last=-1;};
    static std::array<BridgeGate,3> gates{};
    auto& capture_gate=gates[queue_slot];
    auto& captured_bright=capture_gate.bright;
    auto& captured_onset=capture_gate.onset;
    auto& captured_dark=capture_gate.dark;
    auto& captured_post_transition=capture_gate.post;
    auto& last_post_transition_frame=capture_gate.last;
	const bool is_post_transition = phase[0] == 'p' && phase[1] == 'o';
	bool* captured = phase[0] == 'b' ? &captured_bright
	    : phase[0] == 'o' ? &captured_onset
	    : is_post_transition ? nullptr : &captured_dark;
	if (root) {
        // Root history is rolling, not a once-per-phase capture.
    } else if (is_post_transition) {
		// Distinguish the first late event (often zero coefficient updates)
		// from the subsequent paired update events; never capture more than
		// three different late guest frames per process.
		if (captured_post_transition >= 3 ||
		    (last_post_transition_frame >= 0 && frame >= last_post_transition_frame &&
		     frame - last_post_transition_frame < 24)) return;
	} else if (*captured) {
		return;
	}

	const auto find_image = [&bindings](uint64_t addr, TextureCache::BindingType kind) {
		return std::ranges::find_if(bindings.images, [addr, kind](const auto& b) {
			return b.image_id && !b.placeholder && b.desc.info.data.address == addr &&
			       b.desc.type == kind;
		});
	};
	const auto albedo = std::ranges::find_if(bindings.images, [](const auto& b) {
		return b.image_id && !b.placeholder &&
		       b.desc.info.data.address == GBufferAddress;
	});
	const auto hdr = find_image(HdrAddress, TextureCache::BindingType::Storage);
	if (albedo == bindings.images.end() || hdr == bindings.images.end()) {
        if(root)DemonsPrelightTrace::Record({{"kind","light_bridge_missing_binding"},{"frame",frame},{"submit",submit},
            {"queue",queue},{"before",before},{"albedo_bound",albedo!=bindings.images.end()},{"hdr_bound",hdr!=bindings.images.end()}});
		if (is_post_transition) {
			static uint32_t missing_binding_reports = 0;
			if (missing_binding_reports++ < 4) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_LIGHT_BRIDGE_SKIP] frame={} phase={} reason=binding "
				    "albedo_bound={} hdr_bound={} shader=0x{:016x} submit={}\n",
				    frame, phase, albedo != bindings.images.end(),
				    hdr != bindings.images.end(), shader, submit));
			}
		}
		return;
	}
	auto& textures = context.GetTextureCache();
	auto& albedo_image = textures.GetImage(albedo->image_id);
	auto& hdr_image = textures.GetImage(hdr->image_id);
	if (albedo_image.backing.format != vk::Format::eR32Uint ||
	    hdr_image.backing.format != vk::Format::eR16G16B16A16Sfloat) return;

	const auto snapshot = [&context](const auto& binding, uint32_t texel_bytes) {
		std::vector<uint8_t> raw;
		auto& image = context.GetTextureCache().GetImage(binding.image_id);
		const auto& backing = image.backing;
		const auto view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
		if (view == image.views.end() || !backing.image || backing.samples != 1 ||
		    backing.image_type != vk::ImageType::e2D ||
		    !(backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
		    view->info.base_level >= backing.mip_levels ||
		    view->info.base_layer >= backing.layers) return raw;
		const auto w = std::max(backing.extent.width >> view->info.base_level, 1u);
		const auto h = std::max(backing.extent.height >> view->info.base_level, 1u);
		if (w < 16 || h < 16) return raw;
		constexpr uint32_t points = 64u;
		auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
		auto [mapped, offset] = download.Map(points * texel_bytes, 16u, true);
		if (!mapped) return raw;
		download.Commit();
		std::array<vk::BufferImageCopy, points> copies {};
		for (uint32_t i = 0; i < points; ++i) {
			auto& c = copies[i];
			c.bufferOffset = offset + uint64_t{i} * texel_bytes;
			c.imageSubresource = {vk::ImageAspectFlagBits::eColor,
			                      view->info.base_level, view->info.base_layer, 1};
			c.imageOffset = {static_cast<int32_t>((i % 8u * 2u + 1u) * w / 16u),
			                 static_cast<int32_t>((i / 8u * 2u + 1u) * h / 16u), 0};
			c.imageExtent = {1u, 1u, 1u};
		}
		image.Download(std::span{copies}, download.Handle(), offset, points * texel_bytes);
		context.GetCommandScheduler().Finish();
		download.Invalidate(offset, points * texel_bytes);
		raw.resize(points * texel_bytes);
		std::memcpy(raw.data(), mapped, raw.size());
		return raw;
	};
	struct BridgeBefore {
        int32_t frame=-1;
        uint64_t submit=0,gbuffer_owner=0,hdr_owner=0,gbuffer_version=0,hdr_version=0;
        std::array<uint32_t,64> g{};
        std::array<uint16_t,256> h{};
        std::array<std::array<uint32_t,64>,3> other{};
        std::array<uint32_t,64> depth{};
        bool other_valid=false,depth_valid=false;
        nlohmann::json albedo_identity,hdr_identity,depth_identity;
        std::array<uint8_t,64> covered{};
        bool queue_valid=false;
        uint32_t queue_first=0,queue_end=0;
        std::vector<uint32_t> queue_tiles;
        nlohmann::json inputs;
    };
    static thread_local std::array<BridgeBefore,3> before_states;
    auto& previous=before_states[queue_slot];
    const auto gbytes = snapshot(*albedo, 4u);
    const auto hbytes = snapshot(*hdr, 8u);
	// Read the other three material layers at the SAME 8x8 coordinates.
	// The first-layer-only capture cannot distinguish missing geometry from
	// incorrect R32_UINT layer/view selection. Keep these readbacks bounded
	// by the existing bridge windows. Read actual inputs before dispatch too.
	std::array<std::array<uint32_t, 64>, 3> other_gbuffer_layers {};
	std::array<uint32_t, 3> other_gbuffer_colored {};
	bool other_gbuffer_valid = false;
	const auto albedo_view = std::ranges::find(
	    albedo_image.views, albedo->image_view, &CachedImageView::view);
	if (albedo_view != albedo_image.views.end() &&
	    albedo_image.backing.layers >= albedo_view->info.base_layer + 4u &&
	    albedo_image.backing.samples == 1 &&
	    (albedo_image.backing.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
		const auto w = std::max(albedo_image.backing.extent.width >>
		    albedo_view->info.base_level, 1u);
		const auto h = std::max(albedo_image.backing.extent.height >>
		    albedo_view->info.base_level, 1u);
		auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
		auto [mapped, offset] = download.Map(3u * 64u * sizeof(uint32_t), 16u, true);
		if (mapped) {
			download.Commit();
			std::array<vk::BufferImageCopy, 192> copies {};
			for (uint32_t layer = 0; layer < 3; ++layer) {
				for (uint32_t i = 0; i < 64; ++i) {
					auto& copy = copies[layer * 64u + i];
					copy.bufferOffset = offset + (layer * 64u + i) * sizeof(uint32_t);
					copy.imageSubresource = {vk::ImageAspectFlagBits::eColor,
					    albedo_view->info.base_level,
					    albedo_view->info.base_layer + layer + 1u, 1};
					copy.imageOffset = {
					    static_cast<int32_t>(((i % 8u) * 2u + 1u) * w / 16u),
					    static_cast<int32_t>(((i / 8u) * 2u + 1u) * h / 16u), 0};
					copy.imageExtent = {1u, 1u, 1u};
				}
			}
			albedo_image.Download(std::span{copies}, download.Handle(), offset,
			                      3u * 64u * sizeof(uint32_t));
			context.GetCommandScheduler().Finish();
			download.Invalidate(offset, 3u * 64u * sizeof(uint32_t));
			for (uint32_t layer = 0; layer < 3; ++layer) {
				std::memcpy(other_gbuffer_layers[layer].data(),
				            mapped + layer * 64u * sizeof(uint32_t),
				            64u * sizeof(uint32_t));
				for (const uint32_t word: other_gbuffer_layers[layer])
					other_gbuffer_colored[layer] += (word & 0x00ffffffu) != 0u;
			}
			other_gbuffer_valid = true;
		}
	}

	// Depth is copied from the queue's actual bound sampled surface, not from
	// a separately captured material draw in a potentially different frame.
	std::array<uint32_t, 64> depth_bits {};
	bool depth_valid = false;
	nlohmann::json depth_identity;
	uint32_t depth_nonzero = 0;
	const auto depth_binding = std::ranges::find_if(bindings.images, [](const auto& b) {
		return b.image_id && !b.placeholder &&
		       b.desc.info.data.address == 0x282b60000ull;
	});
	if (depth_binding != bindings.images.end()) {
		auto& depth_image = textures.GetImage(depth_binding->image_id);
		const auto depth_view = std::ranges::find(
		    depth_image.views, depth_binding->image_view, &CachedImageView::view);
		const auto format = depth_image.backing.format;
		if (depth_view != depth_image.views.end() &&
		    depth_image.backing.samples == 1 &&
		    (format == vk::Format::eD32Sfloat ||
		     format == vk::Format::eD32SfloatS8Uint) &&
		    (depth_image.backing.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
			const auto w = std::max(depth_image.backing.extent.width >>
			    depth_view->info.base_level, 1u);
			const auto h = std::max(depth_image.backing.extent.height >>
			    depth_view->info.base_level, 1u);
			if (w >= 16 && h >= 16) {
				auto& download = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
				auto [mapped, offset] = download.Map(64u * sizeof(uint32_t), 16u, true);
				if (mapped) {
					download.Commit();
					std::array<vk::BufferImageCopy, 64> copies {};
					for (uint32_t i = 0; i < 64; ++i) {
						auto& copy = copies[i];
						copy.bufferOffset = offset + i * sizeof(uint32_t);
						copy.imageSubresource = {vk::ImageAspectFlagBits::eDepth,
						    depth_view->info.base_level, depth_view->info.base_layer, 1};
						copy.imageOffset = {
						    static_cast<int32_t>(((i % 8u) * 2u + 1u) * w / 16u),
						    static_cast<int32_t>(((i / 8u) * 2u + 1u) * h / 16u), 0};
						copy.imageExtent = {1u, 1u, 1u};
					}
					depth_image.Download(std::span{copies}, download.Handle(), offset,
					                     64u * sizeof(uint32_t));
					context.GetCommandScheduler().Finish();
					download.Invalidate(offset, 64u * sizeof(uint32_t));
					std::memcpy(depth_bits.data(), mapped, 64u * sizeof(uint32_t));
					for (const auto bits: depth_bits) depth_nonzero += bits != 0u;
					depth_valid = true;
					depth_identity=DemonsPrelightTrace::Identity(depth_image);
					depth_identity["layer"]=depth_view->info.base_layer;
					depth_identity["mip"]=depth_view->info.base_level;
				}
			}
		}
	}
    if(before){
        previous={};
        if(gbytes.size()!=256u||hbytes.size()!=512u)return;
        previous.frame=frame;previous.submit=submit;
        previous.gbuffer_owner=albedo_image.info.data.address;previous.hdr_owner=hdr_image.info.data.address;
        previous.gbuffer_version=albedo_image.ContentVersion();previous.hdr_version=hdr_image.ContentVersion();
        std::memcpy(previous.g.data(),gbytes.data(),gbytes.size());std::memcpy(previous.h.data(),hbytes.data(),hbytes.size());
        previous.other=other_gbuffer_layers;previous.other_valid=other_gbuffer_valid;
        previous.depth=depth_bits;previous.depth_valid=depth_valid;previous.depth_identity=depth_identity;
        previous.albedo_identity=DemonsPrelightTrace::Identity(albedo_image);
        previous.hdr_identity=DemonsPrelightTrace::Identity(hdr_image);
        if(albedo_view!=albedo_image.views.end()){
            previous.albedo_identity["layer"]=albedo_view->info.base_layer;
            previous.albedo_identity["mip"]=albedo_view->info.base_level;
        }
        // Read actual bound tile-list/prefix, not the periodically cached queue sample.
        std::vector<uint32_t> prefix,tiles;
        for(uint32_t slot=0;slot<bindings.buffer_sources.size();++slot){
            const auto addr=bindings.buffer_sources[slot].address;
            if(addr==PrefixAddress)prefix=ReadDsBoundWords(context,bindings,slot,256);
            if(addr==TilesAddress)tiles=ReadDsBoundWords(context,bindings,slot,128*1024);
        }
        // The sparse pass uses the same actual prefix/list, queue index 8.
        // If the bindings differ, retain unavailable instead of inferring coverage.
        const auto selected=SelectTiles(prefix,tiles,queue);
        const auto& snap=*bindings.runtime->resources;
        previous.inputs={{"srt",snap.flattened_srt},{"user_data",snap.user_data},{"shader_data",bindings.shader_data}};
        auto buffers=nlohmann::json::array(),images=nlohmann::json::array(),samplers=nlohmann::json::array();
        for(const auto& a:snap.buffers)buffers.push_back(a.dwords);
        for(const auto& a:snap.images)images.push_back(a.dwords);
        for(const auto& a:snap.samplers)samplers.push_back(a.dwords);
        previous.inputs["buffers"]=std::move(buffers);previous.inputs["images"]=std::move(images);
        previous.inputs["samplers"]=std::move(samplers);
        auto resources=nlohmann::json::array();
        for(const auto& res:bindings.runtime->program->info.buffers){
            const auto slot=res.descriptor_index;
            if(slot>=bindings.buffer_sources.size()||slot>=bindings.buffers.size())continue;
            const auto& src=bindings.buffer_sources[slot];
            auto item=nlohmann::json{{"slot",slot},{"source",res.source},{"guest",src.address},{"bytes",src.size},
                {"written",res.written},{"native_offset",bindings.buffers[slot].offset},{"native_range",bindings.buffers[slot].range}};
            // Keep small, complete readonly inputs; larger inputs explicitly
            // retain only a bounded prefix, not an alleged full replay.
            if(!res.written&&src.size&&src.size%4==0&&src.address!=PrefixAddress&&src.address!=TilesAddress){
                const auto words=ReadDsBoundWords(context,bindings,slot,std::min<uint64_t>(src.size/4,2048));
                item["captured_words"]=words;item["complete"]=words.size()==src.size/4;
            }
            resources.push_back(std::move(item));
        }
        previous.inputs["native_buffers"]=std::move(resources);
        auto views=nlohmann::json::array();
        for(const auto& binding:bindings.images){
            auto item=nlohmann::json{{"guest",binding.desc.info.data.address},{"placeholder",binding.placeholder}};
            if(binding.image_id&&!binding.placeholder){
                const auto& im=context.GetTextureCache().GetImage(binding.image_id);
                const auto v=std::ranges::find(im.views,binding.image_view,&CachedImageView::view);
                item["owner"]=im.info.data.address;item["version"]=im.ContentVersion();
                if(v!=im.views.end())item["view"]={{"format",static_cast<uint32_t>(v->info.format)},
                    {"layer",v->info.base_layer},{"layers",v->info.layer_count},{"mip",v->info.base_level},
                    {"aspect",static_cast<uint32_t>(v->info.aspect)}};
            }
            views.push_back(std::move(item));
        }
        previous.inputs["native_images"]=std::move(views);
        if(selected.valid){previous.queue_first=selected.first;previous.queue_end=selected.end;
            previous.queue_tiles.assign(tiles.begin()+selected.first,tiles.begin()+selected.end);}
        const auto av=std::ranges::find(albedo_image.views,albedo->image_view,&CachedImageView::view);
        const auto hv=std::ranges::find(hdr_image.views,hdr->image_view,&CachedImageView::view);
        if(selected.valid&&av!=albedo_image.views.end()&&hv!=hdr_image.views.end()){
            const auto w=std::max(albedo_image.backing.extent.width>>av->info.base_level,1u);
            const auto h=std::max(albedo_image.backing.extent.height>>av->info.base_level,1u);
            const auto hw=std::max(hdr_image.backing.extent.width>>hv->info.base_level,1u);
            const auto hh=std::max(hdr_image.backing.extent.height>>hv->info.base_level,1u);
            if(w==hw&&h==hh){
                previous.queue_valid=true;
                for(uint32_t i=0;i<64;++i){
                    const auto x=((i%8u)*2u+1u)*w/16u,y=((i/8u)*2u+1u)*h/16u;
                    const auto tile=(y/8u)*((w+7u)/8u)+x/8u;
                    previous.covered[i]=std::find(tiles.begin()+selected.first,tiles.begin()+selected.end,tile)!=tiles.begin()+selected.end;
                }
            }
        }
        return;
    }
	if (gbytes.size() != 256u || hbytes.size() != 512u) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_LIGHT_BRIDGE_SKIP] frame={} phase={} reason=readback albedo={} hdr={}\n",
		    frame, phase, gbytes.size(), hbytes.size()));
		return;
	}
	std::array<uint32_t, 64> gwords {};
	std::array<uint16_t, 256> hwords {};
	std::memcpy(gwords.data(), gbytes.data(), gbytes.size());
	std::memcpy(hwords.data(), hbytes.data(), hbytes.size());
	const auto half = [](uint16_t value) -> double {
		if ((value & 0x8000u) || (value & 0x7c00u) == 0x7c00u) return 0.0;
		const auto exponent = (value >> 10u) & 31u;
		const auto fraction = value & 1023u;
		return exponent == 0 ? std::ldexp(double(fraction), -24)
		                     : std::ldexp(1.0 + double(fraction) / 1024.0, int(exponent) - 15);
	};
	uint32_t colored = 0, lit = 0, colored_lit=0, colored_unlit=0, covered_colored=0, covered_colored_unlit=0, covered_changed=0;
    const bool paired=previous.frame==frame&&previous.submit==submit&&previous.gbuffer_owner==albedo_image.info.data.address&&previous.hdr_owner==hdr_image.info.data.address;
    const bool queue_valid=paired&&previous.queue_valid&&groups[1]==1&&groups[2]==1&&
        groups[0]==previous.queue_tiles.size();
    auto joint=nlohmann::json::array();
	double total = 0.0;
	for (uint32_t i = 0; i < 64; ++i) {
		colored += (gwords[i] & 0x00ffffffu) != 0u;
		const double r = half(hwords[4u * i]);
		const double g = half(hwords[4u * i + 1u]);
		const double b = half(hwords[4u * i + 2u]);
		const double level = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		total += std::min(level, 4.0);
		lit += level >= 0.006;
        const bool has_color=(gwords[i]&0x00ffffffu)!=0u,has_light=level>=0.006;
        colored_lit+=has_color&&has_light;colored_unlit+=has_color&&!has_light;
        const bool covered=queue_valid&&previous.covered[i];
        const bool unchanged=paired&&std::equal(hwords.begin()+4*i,hwords.begin()+4*i+4,previous.h.begin()+4*i);
        covered_colored+=covered&&has_color;
        covered_colored_unlit+=covered&&has_color&&!has_light;
        covered_changed+=covered&&has_color&&!unchanged;
        if(has_color)joint.push_back({{"pixel",i},{"albedo",gwords[i]},{"hdr_level",level},{"queue_membership_known",queue_valid},{"covered",covered},
            {"paired",paired},{"albedo_unchanged",paired&&previous.g[i]==gwords[i]},{"hdr_unchanged",unchanged}});
	}
	const nlohmann::json output {
	    {"frame", frame}, {"phase", phase}, {"submit", submit},
	    {"shader", fmt::format("{:016x}", shader)}, {"queue",queue},{"workgroups", groups},{"workgroups_valid",groups_valid},
	    {"gbuffer_guest", albedo->desc.info.data.address},
	    {"gbuffer_owner", albedo_image.info.data.address},
	    {"gbuffer_version", albedo_image.ContentVersion()},
	    {"gbuffer_extent", {albedo_image.backing.extent.width, albedo_image.backing.extent.height}},
	    {"gbuffer_r32", gwords}, {"gbuffer_colored", colored},
	    {"gbuffer_other_layers_valid", other_gbuffer_valid},
	    {"gbuffer_other_layers_colored", other_gbuffer_colored},
	    {"gbuffer_other_layers_r32", other_gbuffer_layers},
	    {"depth_grid_valid", depth_valid}, {"depth_grid_nonzero", depth_nonzero},
	    {"depth_grid_bits", depth_bits},
	    {"hdr_guest", hdr->desc.info.data.address},
	    {"hdr_owner", hdr_image.info.data.address},
	    {"hdr_version", hdr_image.ContentVersion()},
	    {"hdr_extent", {hdr_image.backing.extent.width, hdr_image.backing.extent.height}},
	    {"hdr_rgba16_bits", hwords}, {"hdr_lit", lit}, {"hdr_mean", total / 64.0},
        {"same_dispatch_before_valid",paired},{"queue_membership_valid",queue_valid},
        {"before_gbuffer",previous.g},{"before_hdr",previous.h},{"before_gbuffer_version",previous.gbuffer_version},
        {"before_hdr_version",previous.hdr_version},{"queue_covered",previous.covered},
        {"before_other_layers_valid",paired&&previous.other_valid},{"before_other_layers_r32",previous.other},
        {"before_depth_grid_valid",paired&&previous.depth_valid},{"before_depth_grid_bits",previous.depth},
        {"before_albedo_identity",paired?previous.albedo_identity:nlohmann::json{}},
        {"before_hdr_identity",paired?previous.hdr_identity:nlohmann::json{}},
        {"before_depth_identity",paired?previous.depth_identity:nlohmann::json{}},
        {"after_albedo_identity",DemonsPrelightTrace::Identity(albedo_image)},
        {"after_hdr_identity",DemonsPrelightTrace::Identity(hdr_image)},
        {"after_depth_identity",depth_identity},
        {"colored_lit",colored_lit},{"colored_unlit",colored_unlit},{"covered_colored",covered_colored},
        {"covered_colored_unlit",covered_colored_unlit},{"covered_colored_changed",covered_changed},{"joint_pixels",joint},
        {"queue_first",previous.queue_first},{"queue_end",previous.queue_end},{"queue_tiles",previous.queue_tiles},
        {"input_bindings_before",paired?previous.inputs:nlohmann::json{}}
	};
    if(root){auto row=output;row["kind"]="light_bridge";row["tick"]=context.GetCommandScheduler().CurrentTick();
        if(p8_window)DemonsPrelightTrace::RecordP8Boundary(frame,row);
        DemonsPrelightTrace::Record(std::move(row));return;}
	const auto folder = Config::GetShaderDumpFolder() / "queue-state";
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	if (ec) return;
	const auto name = fmt::format("ds_light_bridge_{}_f{}_q{}.json", phase, frame,queue);
	std::ofstream file(folder / name, std::ios::trunc);
	if (!file) return;
	file << output;
	file.close();
	if (!file.good()) return;
	if (is_post_transition) {
		++captured_post_transition;
		last_post_transition_frame = frame;
	} else {
		*captured = true;
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_LIGHT_BRIDGE_LAYERS] frame={} phase={} layer0={} layer1={} layer2={} "
	    "layer3={} layers_valid={} depth_nonzero={} depth_valid={}\n",
	    frame, phase, colored, other_gbuffer_colored[0], other_gbuffer_colored[1],
	    other_gbuffer_colored[2], other_gbuffer_valid, depth_nonzero, depth_valid));
	Log::WriteToConsoleAndLog(fmt::format(
	    "[DS_LIGHT_BRIDGE] phase={} frame={} submit={} queue={} gbuffer_colored={}/64 "
	    "hdr_lit={}/64 hdr_mean={:.8f} workgroups={}/{}/{} file={}\n",
	    phase, frame, submit, queue,colored, lit, total / 64.0,
	    groups[0], groups[1], groups[2], name));
}

bool RenderExecutor::ProbeDsMaterialLight(const PreparedBindings& bindings, uint64_t submit,
                                         bool before, const std::array<uint32_t, 3>& groups,
                                         bool groups_valid) {
	using namespace DemonsQueueProbe;
	const auto shader = bindings.runtime->program->shader_hash;
	constexpr uint64_t sparse_lighting = 0x3dfe402653925fbfull;
	const uint32_t consumer = shader == sparse_lighting ? 1u :
	    shader == Shaders[0] ? 2u : shader == Shaders[1] ? 4u : 0u;
	if (!consumer) return false;
	const auto frame = m_context.GetGpu().GetFrameNum();
	const auto pending = [frame, consumer, before](const MaterialTrace& trace) {
		if (!before) return (trace.lighting_in_progress & consumer) != 0;
		return MaterialTracePending(trace.frame, frame) && trace.count &&
		       !(trace.lighting_sampled & consumer);
	};
	if (std::ranges::none_of(m_ds_material_traces, pending)) return false;

	std::vector<uint32_t> range, tiles;
	if (!before) {
		for (const auto& resource : bindings.runtime->program->info.buffers) {
			if (resource.written) continue;
            const auto slot=resource.descriptor_index;
            if(slot>=bindings.buffer_sources.size())continue;
            const auto addr=bindings.buffer_sources[slot].address;
            if(addr==PrefixAddress)range=ReadDsBoundWords(m_context,bindings,slot,256);
            if(addr==TilesAddress)tiles=ReadDsBoundWords(m_context,bindings,slot,128*1024);
		}
	}
	for (auto& trace : m_ds_material_traces) {
		if (!pending(trace)) continue;
		if (before) {
			trace.lighting_in_progress |= consumer;
		} else {
			trace.lighting_in_progress &= ~consumer;
			trace.lighting_sampled |= consumer;
		}
		uint32_t tile_mask = 0;
        const auto queue=DemonsCausalMatrix::LightingQueue(shader);
        const bool range_valid=queue!=UINT32_MAX&&uint64_t{queue}+1<range.size()&&
            range[queue]<=range[queue+1]&&range[queue+1]<=tiles.size();
        const uint32_t tile_count=range_valid?range[queue+1]-range[queue]:0u;
        const bool tiles_valid=!before&&range_valid&&groups_valid&&groups[1]==1&&groups[2]==1&&groups[0]==tile_count;
		if (tiles_valid) {
			const auto list = std::span{tiles}.subspan(range[queue],tile_count);
			for (uint32_t i = 0; i < trace.count; ++i) {
				const auto [x, y] = trace.pixels[i];
				if (std::ranges::find(list, (y / 8u) * ((trace.width+7u)/8u) + x / 8u) != list.end())
					tile_mask |= 1u << i;
			}
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_MATERIAL_LIGHT] frame={} draw_frame={} draw_submit={} light_submit={} "
		    "material=0x{:016x} shader=0x{:016x} phase={} samples={} tiles_valid={} "
		    "tile_mask=0x{:x} tile_count={} groups_valid={} groups={}/{}/{}\n",
		    frame, trace.frame, trace.submit, submit, trace.shader, shader, before ? "before" : "after",
		    trace.count, tiles_valid, tile_mask, tiles_valid ? tile_count : 0u,
		    groups_valid, groups[0], groups[1], groups[2]));
		uint32_t roles = 0;
		for (const auto& binding : bindings.images) {
			const bool albedo = before && binding.desc.info.data.address == GBufferAddress;
			const bool hdr = binding.desc.info.data.address == HdrAddress &&
			                 binding.desc.type == TextureCache::BindingType::Storage;
			if ((!albedo && !hdr) || !binding.image_id || binding.placeholder) continue;
			const auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
			const auto view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
			if (view == image.views.end() || view->info.base_layer != 0 || view->info.base_level != 0 ||
			    image.backing.extent.width != trace.width ||
			    image.backing.extent.height != trace.height) continue;
			const uint32_t role = albedo ? 1u : 2u;
			if (roles & role) continue;
			roles |= role;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_MATERIAL_LIGHT_IMAGE] frame={} material=0x{:016x} phase={} role={} "
			    "owner=0x{:x} view_format={}\n", frame, trace.shader, before ? "before" : "after",
			    albedo ? "albedo" : "hdr", image.info.data.address,
			    static_cast<uint32_t>(view->info.format)));
			std::array<uint32_t, DsProbePixels{}.size()> words;
			words.fill(UINT32_MAX);
			const auto version = image.ContentVersion();
			ProbeDsComputeImage(m_context, binding.image_id, shader, true, &view->info,
			                    &trace.pixels, albedo ? &words : nullptr);
			if (albedo) {
				const bool same_image = trace.image_index == binding.image_id.index &&
				    trace.image_generation == binding.image_id.generation;
				uint32_t same = 0;
				for (uint32_t i = 0; i < trace.count; ++i) {
					if (words[i] == trace.output[i]) same |= 1u << i;
				}
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_MATERIAL_HANDOFF] draw_frame={} frame={} material=0x{:016x} "
				    "shader=0x{:016x} same_image={} draw_version={} read_version={} same_words=0x{:x}\n",
				    trace.frame, frame, trace.shader, shader, same_image,
				    trace.content_version, version, same));
			}
		}
		if (roles != (before ? 3u : 2u)) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_MATERIAL_LIGHT_MISSING] frame={} material=0x{:016x} phase={} roles=0x{:x}\n",
			    frame, trace.shader, before ? "before" : "after", roles));
		}
	}
	return true;
}

static void SaveDsQueueFile(const std::string& name, const char* subdir, const void* data,
                            size_t size) {
	const auto root = Config::GetShaderDumpFolder();
	if (root.filename() != "ds-lighting-dumps") return;
	const auto folder = root / subdir;
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	if (!ec) {
		std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
		if (file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size))) return;
	}
	Log::WriteToConsoleAndLog(fmt::format("[DS_QUEUE_CAPTURE_ERROR] file={} reason=write_failed\n", name));
}

void RenderExecutor::CaptureDsLightTiles(const PreparedBindings& bindings) {
	using namespace DemonsQueueProbe;
	if (!DsTraceEnabled() || bindings.runtime->program->shader_hash != TileProducer) return;
	const auto frame = m_context.GetGpu().GetFrameNum();
	if (m_ds_tile_frame == frame) return;
	static DemonsRollingProbe::FrameGate gate;
	const bool paired = ds_last_coefficient_pair == frame ||
	    (ds_radiance_writer.frame == frame &&
	     ds_radiance_writer.dispatch.contains("radiance_digest"));
	if (!gate.Due(frame, 240) && !paired) return;
	m_ds_tile_frame = frame;
	std::vector<uint32_t> offsets, tiles;
	for (const auto& resource : bindings.runtime->program->info.buffers) {
		const auto slot = resource.descriptor_index;
		if (!resource.written || slot >= bindings.buffer_sources.size()) continue;
		const auto addr = bindings.buffer_sources[slot].address;
		if (addr == PrefixAddress) offsets = ReadDsBoundWords(m_context, bindings, slot, 256);
		if (addr == TilesAddress) tiles = ReadDsBoundWords(m_context, bindings, slot, 128 * 1024);
	}
	for (size_t i = 0; i < Queues.size(); ++i) {
		const auto selected = SelectTiles(offsets, tiles, Queues[i]);
		m_ds_light_tiles[i] = selected;
		auto& selected_ids = m_ds_light_tile_ids[i];
		selected_ids.clear();
		if (selected.valid)
			selected_ids.assign(tiles.begin() + selected.first, tiles.begin() + selected.end);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_QUEUE_TILES] frame={} queue={} first={} end={} prefix_words={} list_words={} valid={} "
		    "ids={},{},{},{},{} radiance_paired={}\n", frame, Queues[i], selected.first, selected.end,
		    offsets.size(), tiles.size(), selected.valid, selected.ids[0], selected.ids[1],
		    selected.ids[2], selected.ids[3], selected.ids[4], paired));
	}
	const auto state = nlohmann::json {
	    {"frame", frame}, {"prefix_words", offsets.size()}, {"tile_words", tiles.size()}
	}.dump();
	SaveDsQueueFile(fmt::format("queue_{:016x}_tiles.json", TileProducer),
	                "queue-state", state.data(), state.size());
	if (!offsets.empty()) SaveDsQueueFile(fmt::format("queue_{:016x}_prefix.bin", TileProducer),
	    "queue-state", offsets.data(), offsets.size() * sizeof(uint32_t));
	if (!tiles.empty()) SaveDsQueueFile(fmt::format("queue_{:016x}_tiles.bin", TileProducer),
	    "queue-state", tiles.data(), tiles.size() * sizeof(uint32_t));
}

static void ProbeDsQueueImages(RenderContext& context, const PreparedBindings& bindings,
                               const DemonsQueueProbe::Capture& capture, uint32_t queue,
                               bool before) {
	const auto& resources = bindings.runtime->program->info.images;
	for (size_t i = 0; i < bindings.images.size() && i < resources.size(); ++i) {
		const auto& binding = bindings.images[i];
		const auto addr = binding.desc.info.data.address;
		if (!binding.image_id || binding.placeholder) {
			if (before) Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_QUEUE_IMAGE] frame={} queue={} index={} guest=0x{:x} missing=1\n",
			    capture.frame, queue, i, addr));
			continue;
		}
		if (!before && !resources[i].written) continue;
		const auto& image = context.GetTextureCache().GetImage(binding.image_id);
		const auto view = std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
		if (view == image.views.end()) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_QUEUE_IMAGE] frame={} queue={} phase={} index={} guest=0x{:x} missing_view=1\n",
			    capture.frame, queue, before ? "before" : "after", i, addr));
			continue;
		}
		const bool screen_extent = (image.backing.extent.width >> view->info.base_level) == capture.width &&
		                           (image.backing.extent.height >> view->info.base_level) == capture.height;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_QUEUE_IMAGE] frame={} queue={} phase={} index={} guest=0x{:x} owner=0x{:x} "
		    "format={} mip={} layer={} aspect=0x{:x} written={} probe={}\n",
		    capture.frame, queue, before ? "before" : "after", i, addr, image.info.data.address,
		    static_cast<uint32_t>(image.backing.format), view->info.base_level, view->info.base_layer,
		    static_cast<uint32_t>(view->info.aspect), resources[i].written, screen_extent));
		if (screen_extent)
			ProbeDsComputeImage(context, binding.image_id, capture.shader, true, &view->info,
			                    &capture.pixels);
	}
}

static void SaveDsQueueBindings(RenderContext& context, const PreparedBindings& bindings,
                                const DemonsQueueProbe::Capture& capture, uint32_t queue) {
	const auto& snapshot = *bindings.runtime->resources;
	const auto descriptors = [](const auto& items) {
		auto array = nlohmann::json::array();
		for (const auto& item : items) {
			array.push_back(std::vector<uint32_t>(item.dwords.begin(),
			    item.dwords.begin() + std::min<size_t>(item.dwords.size(), item.dword_count)));
		}
		return array;
	};
	const auto prefix = fmt::format("queue_{:016x}_", capture.shader);
	nlohmann::json state {
	    {"frame", capture.frame}, {"queue", queue}, {"width", capture.width}, {"height", capture.height},
	    {"pixels", capture.pixels}, {"guest_buffers", descriptors(snapshot.buffers)},
	    {"guest_images", descriptors(snapshot.images)}, {"guest_samplers", descriptors(snapshot.samplers)},
	    {"flattened_srt", snapshot.flattened_srt}, {"user_data", snapshot.user_data},
	    {"shader_data", bindings.shader_data}
	};
	auto& captured = state["bound_buffers"] = nlohmann::json::array();
	for (const auto& resource : bindings.runtime->program->info.buffers) {
		const auto slot = resource.descriptor_index;
		if (slot >= bindings.buffer_sources.size()) continue;
		const auto& source = bindings.buffer_sources[slot];
		const auto name = fmt::format("{}slot{}.bin", prefix, slot);
		// Screen-size lists are exported at the producer. For other resources preserve
		// small constants and headers, with explicit byte counts for truncated buffers.
		std::vector<uint32_t> words;
		if (!resource.written && source.address != DemonsQueueProbe::PrefixAddress &&
		    source.address != DemonsQueueProbe::TilesAddress) {
			words = ReadDsBoundWords(context, bindings, slot, 4096);
			if (!words.empty()) SaveDsQueueFile(name, "queue-state", words.data(),
			                                    words.size() * sizeof(uint32_t));
		}
		captured.push_back({{"slot", slot}, {"source", resource.source}, {"address", source.address},
		                    {"bytes", source.size}, {"captured_bytes", words.size() * sizeof(uint32_t)},
		                    {"file", words.empty() ? std::string {} : name}, {"written", resource.written}});
	}
	const auto json = state.dump();
	SaveDsQueueFile(prefix + "bindings.json", "queue-state", json.data(), json.size());
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode, bool async_compute) {
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopDrawOperations();
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();
	const bool  use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;

	ShaderComputeInputInfo input_info {};
	ShaderProgram          compute_program;
	if (auto* prepared = GpuPipeline::t_prepared_compute; prepared != nullptr) {
		// KYTY_GPU_PIPELINE: the front walked this dispatch's SRT (PrefetchDispatchDirect).
		input_info      = prepared->input_info;
		compute_program = prepared->program;
	} else {
		EXIT_IF(GpuPipeline::OnBack() && WalkFan::t_prewalk == nullptr); // walks there only with KYTY_GPU_FAN
		input_info = DirectDispatchInputInfo(cs_regs, thread_group_x, thread_group_y,
		                                     thread_group_z, mode, async_compute);
		PipelineProbe::BeginItem();
		compute_program =
		    m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info);
		NoteComputeItem(input_info);
	}
	const PipelineProbe::ScopedRecordPhase record_phase;
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	if (LiveKnobs::SkipShader(program.shader_hash)) {
		ResetBindings();
		return;
	}
	if (resources.specialization_reads.empty()) {
		// Cached metadata addresses can be reused as ordinary buffers; preserve their guest writes.
		const bool meta_cleared=UpdateComputeMetaClears(input_info, buffer);
		static const bool keep_meta_dispatch=[] {const char* v=std::getenv("KYTY_COMPUTE_META_CLEAR_DISPATCH");return v&&*v&&std::strcmp(v,"0")!=0;}();
		// U59 consumes a recorded exact metadata clear by default; do not execute
		// its raw buffer writes again. Retain the original Android behavior outside DS.
		if(DemonsPcRenderParity::ConsumeMetaDispatch(DemonsPcRenderParity::Enabled(),meta_cleared,keep_meta_dispatch)) {
			ResetBindings();return;
		}
		if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
		                               thread_group_z, mode)) {
			ResetBindings();
			return;
		}
	}

	if (use_thread_dimensions) {
		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = input_info.workgroup_counts[0];
		thread_group_y       = input_info.workgroup_counts[1];
		thread_group_z       = input_info.workgroup_counts[2];

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering(PassEnd::Compute);
	auto& pipeline =
	    m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	if (DemonsCausalMatrix::Mode()==DemonsCausalMatrix::PassOrder &&
        DemonsCausalMatrix::OrderedShader(input_info.stage.program->shader_hash)) {
        m_context.GetCommandScheduler().Finish();
        static uint32_t logged=0;
        if(logged++<64) Log::WriteToConsoleAndLog(fmt::format("[DS_MATRIX_ORDER] mode=D2 shader={:016x} submit={}\n",input_info.stage.program->shader_hash,submit_id));
    }
	const uint64_t ds_preparation_tick = m_context.GetCommandScheduler().CurrentTick();
	PrepareBindings(input_info.stage, bindings);
	if (program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) {
		std::copy(std::begin(input_info.dispatch_threads_num), std::end(input_info.dispatch_threads_num),
		          bindings.shader_data.begin() + program.bindings.dispatch_thread_dword);
	}
	PreparedBindings* descriptor_stage = &bindings;
	if (program.info.uses_dma)
		PrefaultDmaPointers(m_context.GetBufferCache(), input_info.stage);
	FindBuffers(std::span {&descriptor_stage, 1u});

	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	if (program.info.bda_writes) {
		m_context.GetBufferCache().PrepareBdaWrites();
	}
	RebindImages(bindings);
	BindSharedMemory(m_context, input_info, bindings);
	BindDsLightMath(bindings, nullptr);
	m_rebind_epoch = m_context.GetBufferCache().UnregisterEpoch();
	RebindBuffers(bindings);
	if (RebindAfterJoinEnabled()) {
		auto& cache = m_context.GetBufferCache();
		for (uint32_t pass = 0; pass < 4 && cache.UnregisterEpoch() != m_rebind_epoch; ++pass) {
			NoteRebindAfterJoin("dispatch", nullptr);
			m_rebind_epoch = cache.UnregisterEpoch();
			RebindBuffers(bindings);
		}
	}

	auto* post = BeginDsPostCompute(bindings, submit_id);
	const bool material_probe = ProbeDsMaterialLight(bindings, submit_id, true);
	auto coefficient_trace = BeginDsCoefficientDispatch(m_context, bindings, submit_id);
	auto count_producer_trace = BeginDsCountProducer(m_context, bindings, submit_id);
	if (count_producer_trace) BindDsCountExec(bindings,true);
    const auto relight_trace=SelectDsRelightTrace(m_context,bindings,{thread_group_x,thread_group_y,thread_group_z},submit_id);
    if(relight_trace.header[0])BindDsRelightTrace(bindings,relight_trace.header);
    DsVertexProducerCapture vertex_producer; // V3: no W0 capture
    DemonsPrelightTrace::ObserveCompute(m_context,bindings,submit_id,false,false);
    if(DemonsPrelightTrace::Enabled()&&DemonsCausalMatrix::LightingQueue(program.shader_hash)!=UINT32_MAX)
        CaptureDsLightBridge(m_context,bindings,submit_id,{thread_group_x,thread_group_y,thread_group_z},true,true);
	auto indirect_producer = BeginDsIndirectProducer(m_context, bindings, input_info, submit_id,
	    compute_program.module, {thread_group_x, thread_group_y, thread_group_z});
	auto              vk_buffer        = buffer.Handle();
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	const bool ds_bvh_dependency = DemonsLightingMemoryPolicy::NeedsBvhProducerBarrier(
	    DemonsBvhSrtCoherence(), program.info.bvh);
	if (has_storage_writes || ds_bvh_dependency) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer,
		    ds_bvh_dependency ? vk::PipelineStageFlagBits::eAllCommands
		                      : vk::PipelineStageFlagBits::eComputeShader);
	}
	if (Config::GraphicsDebugDumpEnabled()) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " tick=%" PRIu64 " completed_tick=%" PRIu64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, program.shader_hash,
		     m_context.GetCommandScheduler().CurrentTick(),
		     m_context.GetCommandScheduler().GetMasterSemaphore().KnownGpuTick(),
		     thread_group_x, thread_group_y, thread_group_z, mode,
		     input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(
			    resources.samplers[program.info.samplers[i].snapshot_index]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	buffer.MarkGpu("misc");
	{
		const ScopedDebugLabel label(vk_buffer, "cs", program.shader_hash);
		// A packed program runs group_pack guest groups along X per host group (x is even).
		const uint32_t host_x = thread_group_x / std::max(program.info.group_pack, 1u);
		RecordGuestDispatch(vk_buffer, ComputeGroupLimits(m_context.GetGraphics()),
		                    {host_x, thread_group_y, thread_group_z}, program, pipeline);
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::Dispatches);
	m_context.GetCommandScheduler().NoteWork();

	buffer.MarkGpu("cs", program.shader_hash);
	RetagDsRecordedComputeWrites(m_context, bindings, ds_preparation_tick);
	RememberDsBufferProducers(m_context, bindings, false, submit_id);

	// Record the shader barriers and diagnostics before the BDA settle submits the
	// current command buffer; the Vulkan handle is no longer recordable after Wait.
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
    EndDsIndirectProducer(m_context, std::move(indirect_producer));
	EndDsCountProducer(m_context,bindings,std::move(count_producer_trace),
	                   {thread_group_x,thread_group_y,thread_group_z});
    SaveDsRelightTrace(m_context,bindings,submit_id,relight_trace);
	CaptureDsGeometryWriter(m_context, bindings, submit_id,
	                           {thread_group_x, thread_group_y, thread_group_z});
	CaptureDsRelightMaskWriter(m_context, bindings, submit_id,
	                           {thread_group_x, thread_group_y, thread_group_z});
	CaptureDsRadianceComputeWriter(m_context, bindings, submit_id,
	                               {thread_group_x, thread_group_y, thread_group_z});
	CaptureDsProbeSourceWrite(m_context, bindings, submit_id,
	                         {thread_group_x, thread_group_y, thread_group_z});
	CaptureDsRayHitProducer(m_context, bindings, submit_id,
	                        {thread_group_x, thread_group_y, thread_group_z}, true);
	EndDsCoefficientDispatch(m_context, bindings, std::move(coefficient_trace),
	                         {thread_group_x, thread_group_y, thread_group_z});
	if (post) ProbeDsPostImages(bindings, *post, submit_id, program.shader_hash, true, false);
	if (material_probe) {
		ProbeDsMaterialLight(bindings, submit_id, false,
		    {thread_group_x / std::max(program.info.group_pack, 1u), thread_group_y, thread_group_z},
		    true);
	}
	ProbeDsComputeWrites(m_context, input_info, bindings);
	CaptureDsLightTiles(bindings);
	ProbeDsLightingBuffers(m_context, bindings);
	ProbeDsAtlasInputs(m_context, bindings, submit_id,
	                   {thread_group_x, thread_group_y, thread_group_z});
	if (program.info.bda_writes) {
		m_context.GetBufferCache().SettleBdaWrites(program.shader_hash);
	}
	EndDsVertexProducer(m_context,std::move(vertex_producer));
    if(DemonsPrelightTrace::Enabled()&&DemonsCausalMatrix::LightingQueue(program.shader_hash)!=UINT32_MAX)
        CaptureDsLightBridge(m_context,bindings,submit_id,{thread_group_x,thread_group_y,thread_group_z},true);
	DemonsPrelightTrace::ObserveCompute(m_context,bindings,submit_id,true,false);
	ResetBindings();
}

// Thread-dimension dispatches also need the unrounded counts on the CPU to
// bound partial workgroups and materialize descriptors. Read the actual Vulkan
// argument buffer when guest backing is no longer authoritative.
static vk::DispatchIndirectCommand ReadIndirectThreadCounts(RenderContext& context,
                                                            uint64_t address) {
	Common::LockGuard lock(context.GetMutex());
	const Common::PerfStats::ScopedWaitSite wait(Common::PerfStats::Wait::SemIndirectArgs);
	auto& cache = context.GetBufferCache();
	auto& textures = context.GetTextureCache();
	constexpr auto size = sizeof(vk::DispatchIndirectCommand);
	const bool image_gpu_owned = textures.IsRegionGpuModified(address, size);
	vk::DispatchIndirectCommand args {};
	if (!image_gpu_owned && !cache.HasGpuDirtyBytes(address, size) &&
	    !cache.IsRegionGpuModified(address, size) &&
	    !cache.HasPendingBackingPublication(address, size) &&
	    LibKernel::Memory::TryReadBacking(address, &args, size)) {
		return args;
	}
	auto& scheduler = context.GetCommandScheduler();
	scheduler.EndRendering(PassEnd::Compute);
	if (image_gpu_owned) {
		(void)textures.PublishGpuImageForIndirectArgs(address, size);
	}
	auto& download = cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, dst] = download.Map(size, 16u, true);
	EXIT_IF(mapped == nullptr);
	download.Commit();
	const auto [source, offset] = cache.ObtainBuffer(address, size, false);
	EXIT_IF(source == nullptr || (offset & 3u) != 0);
	download.CopyFrom(scheduler.Current(), *source, offset, dst, size,
	                  vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite,
	                  vk::AccessFlagBits::eHostRead, vk::AccessFlagBits::eMemoryRead,
	                  vk::AccessFlagBits::eHostRead);
	scheduler.Finish();
	download.Invalidate(dst, size);
	std::memcpy(&args, mapped, size);
	if (DsTraceEnabled()) {
		const auto frame = context.GetGpu().GetFrameNum();
		const auto queue_offset = address - ds_light_queue_writer.address;
		if (ds_light_queue_writer.frame == frame && queue_offset < ds_light_queue_writer.size &&
		    queue_offset % size == 0) {
			static std::array<DemonsRollingProbe::FrameGate, 65> queues {};
			const auto index = queue_offset / size;
			if (queues[index].Due(frame, 240)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_LIGHT_QUEUE_THREADS] frame={} index={} args=0x{:x} x={} y={} z={}\n",
				    frame, index, address, args.x, args.y, args.z));
			}
		}
	}
	return args;
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode, bool async_compute) {
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0);
	m_context.GetCommandScheduler().PopDrawOperations();
	if (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) {
		const auto args = ReadIndirectThreadCounts(m_context, args_addr);
		DispatchDirect(submit_id, buffer, args.x, args.y, args.z, mode, async_compute);
		return;
	}
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	vk::DispatchIndirectCommand args {};
	auto& indirect_cache = m_context.GetBufferCache();
	// The buffer ledger alone cannot prove CPU backing is current: a GPU
	// image can own these bytes. Demon's Souls keeps the Vulkan indirect
	// arguments authoritative when an overlapping image is GPU-modified.
	// This check never changes the GPU's workgroup counts.
	static const bool ds_gpu_zero_indirect = [] {
		const char* value = std::getenv("KYTY_DS_GPU_ZERO_INDIRECT");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	const bool image_gpu_owned = ds_gpu_zero_indirect &&
	    m_context.GetTextureCache().IsRegionGpuModified(args_addr, sizeof(args));
	if (image_gpu_owned) {
		// Selecting the Vulkan indirect path is not enough when a previous shader
		// wrote these bytes through an image alias. ObtainBuffer can otherwise
		// return an old GPU buffer (or stream an old CPU copy) with zero groups.
		// Publish the owning image into the buffer cache on the GPU timeline first.
		// Unsupported or ambiguous aliases retain the original behavior.
		buffer.EndRendering(PassEnd::Compute);
		(void)m_context.GetTextureCache().PublishGpuImageForIndirectArgs(args_addr, sizeof(args));
	}
	const bool host_args_current =
	    !indirect_cache.HasGpuDirtyBytes(args_addr, sizeof(args)) &&
	    !indirect_cache.IsRegionGpuModified(args_addr, sizeof(args)) &&
	    !(ds_gpu_zero_indirect && indirect_cache.HasPendingBackingPublication(args_addr, sizeof(args))) &&
	    !image_gpu_owned &&
	    LibKernel::Memory::TryReadBacking(args_addr, &args, sizeof(args));
	const bool host_zero = args.x == 0 || args.y == 0 || args.z == 0;
	const auto args_source =
	    ChooseIndirectDispatchSource(host_args_current, host_zero, ds_gpu_zero_indirect);
	if (args_source == IndirectDispatchSource::Skip) {
		return;
	}
	const bool cpu_args = args_source == IndirectDispatchSource::Cpu;
	const auto limits = ComputeGroupLimits(m_context.GetGraphics());
	// Arguments the CPU knows past the host limits: recorded as direct vkCmdDispatchBase pieces.
	// GPU-written ones: clamped to the limits by a fix-up pass (logged when it clamps).
	const bool split_on_cpu = cpu_args && !DispatchFits({args.x, args.y, args.z}, limits);
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	input_info.indirect_args_address = args_addr;
	input_info.bvh_live              = LiveBvhOptions();
	input_info.lane_branch_live      = LiveLaneBranches();
	input_info.dead_lane_live        = LiveDeadLane();
	input_info.rt_half_live          = RtHalfFor(LiveRtHalf(), m_context.GetBufferCache().RetraceBuffer() != nullptr);
	input_info.rt_fill_live          = LiveRtFill();
	if ((input_info.rt_half_live == 3u || (input_info.rt_half_live == 0u && HalfRateTiles::EnvMode() == HalfRateTiles::kModeAdaptive)) &&
	    input_info.rt_fill_live == HalfRateTiles::kFillRetracePass) {
		input_info.rt_fill_live = HalfRateTiles::kFillNearest4; // that code marks pass 2 of rt_half=2
	}
	const auto pass2_input = input_info; // rt_half=2 pass 2 starts from the same inputs
	PipelineProbe::BeginItem();
	const auto compute_program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info);
	NoteComputeItem(input_info);
	const PipelineProbe::ScopedRecordPhase record_phase;
	if (input_info.stage.program != nullptr &&
	    LiveKnobs::SkipShader(input_info.stage.program->shader_hash)) {
		return;
	}
	buffer.EndRendering(PassEnd::Compute);
	// rt_half=2 (two dispatches, shader/halfRateTiles.h): pass 1 starts an empty compact list.
	auto* const retrace = m_context.GetBufferCache().RetraceBuffer();
	const bool  two_pass = retrace != nullptr && !HalfRateTiles::LoopDiagnostic() &&
	                      input_info.half_rate_tiles == HalfRateTiles::kVariantAdaptive;
	if (two_pass) {
		const std::array<uint32_t, 4> start {0u, 1u, 1u, 0u};
		buffer.Handle().updateBuffer(retrace->Handle(), 0, sizeof(start), start.data());
		vk::MemoryBarrier reset {};
		reset.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		reset.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		buffer.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader, {}, 1,
		                                &reset, 0, nullptr, 0, nullptr);
	}
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	if (DemonsCausalMatrix::Mode()==DemonsCausalMatrix::PassOrder &&
        DemonsCausalMatrix::OrderedShader(input_info.stage.program->shader_hash)) {
        m_context.GetCommandScheduler().Finish();
        static uint32_t logged=0;
        if(logged++<64) Log::WriteToConsoleAndLog(fmt::format("[DS_MATRIX_ORDER] mode=D2 shader={:016x} submit={}\n",input_info.stage.program->shader_hash,submit_id));
    }
	const uint64_t ds_preparation_tick = m_context.GetCommandScheduler().CurrentTick();
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	if (input_info.stage.program->info.uses_dma)
		PrefaultDmaPointers(m_context.GetBufferCache(), input_info.stage);
	FindBuffers(std::span {&descriptor_stage, 1u});
	const auto& program = *input_info.stage.program;
	if (DsTraceEnabled()) {
		// The captured producer emits 65 records, including two much larger
		// queues than any of the six originally traced HDR kernels.
		const auto frame = m_context.GetGpu().GetFrameNum();
		const auto& writer = ds_light_queue_writer;
		const auto offset = args_addr - writer.address;
		if (writer.frame == frame && offset < writer.size &&
		    offset % sizeof(vk::DispatchIndirectCommand) == 0) {
			const auto index = offset / sizeof(vk::DispatchIndirectCommand);
			static std::array<DemonsRollingProbe::FrameGate, 65> queues {};
			if (queues[index].Due(frame, 240)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_LIGHT_QUEUE] frame={} index={} shader=0x{:016x} args=0x{:x}\n",
				    frame, index, program.shader_hash, args_addr));
				for (size_t i = 0; i < bindings.images.size() && i < program.info.images.size(); ++i) {
					const auto& image = bindings.images[i];
					if (!image.image_id || image.placeholder || !program.info.images[i].written) continue;
					Log::WriteToConsoleAndLog(fmt::format(
					    "[DS_LIGHT_QUEUE_OUTPUT] frame={} index={} shader=0x{:016x} guest=0x{:x}\n",
					    frame, index, program.shader_hash, image.desc.info.data.address));
				}
			}
		}
	}
	// Demon's Souls: for the six failing light-dispatch commands, compare
	// original guest backing with the GPU-indirect path's read-only feedback.
	// Unlike a synchronous GPU readback, this cannot modify counts or wait on
	// a new fence. Only bounded samples are exported for init and gameplay.
	if (ds_gpu_zero_indirect && DsTraceEnabled()) {
		constexpr std::array<uint64_t, 6> watched_lighting {{
		    0xc2e01fbe98f5ebabull, 0x23056085c1120461ull,
		    0x3dfe402653925fbfull, 0xfabe0bcb275a3005ull,
		    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
		}};
		if (std::find(watched_lighting.begin(), watched_lighting.end(),
		              program.shader_hash) != watched_lighting.end()) {
			const int32_t frame = m_context.GetGpu().GetFrameNum();
			const size_t ordinal = static_cast<size_t>(
			    std::find(watched_lighting.begin(), watched_lighting.end(),
			              program.shader_hash) - watched_lighting.begin());
			static std::array<DemonsRollingProbe::FrameGate, 6> sampled {};
			if (sampled[ordinal].Due(frame, 32)) {
				vk::DispatchIndirectCommand backing {};
				const bool backing_read = LibKernel::Memory::TryReadBacking(
				    args_addr, &backing, sizeof(backing));
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_ARGS_PROVENANCE] frame={} shader=0x{:016x} addr=0x{:x} "
				    "chosen={} gpu_image={} buffer_dirty={} buffer_modified={} "
				    "cpu_modified={} pending_publication={} backing_ok={} "
				    "backing_x={} backing_y={} backing_z={}\n",
				    frame, program.shader_hash, args_addr,
				    cpu_args ? "cpu" : "gpu", image_gpu_owned ? 1 : 0,
				    indirect_cache.HasGpuDirtyBytes(args_addr, sizeof(args)) ? 1 : 0,
				    indirect_cache.IsRegionGpuModified(args_addr, sizeof(args)) ? 1 : 0,
				    indirect_cache.IsRegionCpuModified(args_addr, sizeof(args)) ? 1 : 0,
				    indirect_cache.HasPendingBackingPublication(args_addr, sizeof(args)) ? 1 : 0,
				    backing_read ? 1 : 0, backing.x, backing.y, backing.z));
				for (uint32_t axis = 0; axis < 3; ++axis) {
					LogDsBufferProducer(frame, program.shader_hash, args_addr, "dispatch", axis,
					                    args_addr + axis * sizeof(uint32_t), sizeof(uint32_t));
				}
				if (program.shader_hash == 0x3dfe402653925fbfull) {
					for (const auto& resource: program.info.buffers) {
						if (resource.written || resource.descriptor_index >= bindings.buffer_sources.size())
							continue;
						const auto& source = bindings.buffer_sources[resource.descriptor_index];
						LogDsBufferProducer(frame, program.shader_hash, args_addr, "input",
						                    resource.source, source.address, source.size);
					}
				}
			}
		}
	}
	// A CPU-current indirect command bypasses the Vulkan clamp feedback path.
	// Log its real groups too, otherwise zero GPU samples can wrongly imply
	// that the six lighting kernels never dispatched.
	if (cpu_args) {
		static const bool ds_trace = [] {
			const char* v = std::getenv("KYTY_DS_FRAME_TRACE");
			return v != nullptr && v[0] == '1' && v[1] == '\0';
		}();
		constexpr std::array<uint64_t, 6> lighting_hashes {{
		    0xc2e01fbe98f5ebabull, 0x23056085c1120461ull,
		    0x3dfe402653925fbfull, 0xfabe0bcb275a3005ull,
		    0xe75589aa0bb3ae52ull, 0x93b0be7f1a0293d1ull
		}};
		const bool lighting = ds_trace &&
		    std::find(lighting_hashes.begin(), lighting_hashes.end(), program.shader_hash) !=
		        lighting_hashes.end();
		if (lighting) {
			const int32_t frame = m_context.GetGpu().GetFrameNum();
			const auto ordinal = static_cast<size_t>(
			    std::find(lighting_hashes.begin(), lighting_hashes.end(),
			              program.shader_hash) - lighting_hashes.begin());
			static std::array<DemonsRollingProbe::FrameGate, 6> cpu_samples {};
			if (cpu_samples[ordinal].Due(frame, 32)) {
				Log::WriteToConsoleAndLog(fmt::sprintf(
				    "[DS_LIGHTING_GROUPS] source=cpu frame=%d shader=0x%016" PRIx64
				    " args=0x%016" PRIx64 " x=%u y=%u z=%u saturated=0 clamped=0 zero=0\n",
				    frame, program.shader_hash, args_addr, args.x, args.y, args.z));
			}
		}
	}

	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	if (program.info.bda_writes) {
		m_context.GetBufferCache().PrepareBdaWrites();
	}
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	RebindImages(bindings);
	DemonsQueueProbe::Capture* queue_capture = nullptr;
	size_t captured_queue_index = 0;
	const auto frame = m_context.GetGpu().GetFrameNum();
	if (m_ds_tile_frame == frame && ds_light_queue_writer.frame == frame) {
		using namespace DemonsQueueProbe;
		for (size_t i = 0; i < Queues.size(); ++i) {
			const auto queue = Queues[i];
			if (args_addr != ds_light_queue_writer.address + queue * sizeof(vk::DispatchIndirectCommand) ||
			    program.shader_hash != Shaders[i] || m_ds_queue_captures[i].frame == frame) continue;
			const auto hdr = std::ranges::find_if(bindings.images, [](const auto& binding) {
				return binding.image_id && !binding.placeholder &&
				       binding.desc.info.data.address == HdrAddress &&
				       binding.desc.type == TextureCache::BindingType::Storage;
			});
			if (hdr == bindings.images.end()) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_QUEUE_CAPTURE_ERROR] frame={} queue={} reason=hdr_unbound\n", frame, queue));
				break;
			}
			const auto& image = m_context.GetTextureCache().GetImage(hdr->image_id);
			const auto view = std::ranges::find(image.views, hdr->image_view, &CachedImageView::view);
			if (view == image.views.end()) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DS_QUEUE_CAPTURE_ERROR] frame={} queue={} reason=hdr_view_missing\n", frame, queue));
				break;
			}
			const auto mip = view->info.base_level;
			Capture capture {frame, program.shader_hash, std::max(image.backing.extent.width >> mip, 1u),
			                 std::max(image.backing.extent.height >> mip, 1u)};
			const bool covered = TilePixels(m_ds_light_tiles[i], capture.width, capture.height,
			                               capture.pixels);
			uint32_t colored = 0;
			if (covered) {
				const auto albedo = std::ranges::find_if(bindings.images, [](const auto& binding) {
					return binding.image_id && !binding.placeholder &&
					       binding.desc.info.data.address == GBufferAddress;
				});
				if (albedo != bindings.images.end()) {
					const auto& source = m_context.GetTextureCache().GetImage(albedo->image_id);
					const auto native_view = std::ranges::find(
					    source.views, albedo->image_view, &CachedImageView::view);
					if (native_view != source.views.end())
						colored = PickDsColoredTilePixels(m_context, albedo->image_id, native_view->info,
						    m_ds_light_tile_ids[i], capture.width, capture.height, capture.pixels);
				}
			}
			if (covered && colored != 0 && queue == 33)
				DemonsAdaptiveCapture::Current().MarkGeometry(frame, colored);
			if (!covered) {
				// Still record arguments and bindings for empty or invalid lists.
				const auto w = capture.width;
				const auto h = capture.height;
				capture.pixels = {{{w/2, h/2}, {w/4, h/4}, {w-w/4, h/4},
				                   {w/4, h-h/4}, {w-w/4, h-h/4}}};
			}
			m_ds_queue_captures[i] = capture;
			queue_capture = &m_ds_queue_captures[i];
			captured_queue_index = i;
			const auto& p = capture.pixels;
			Log::WriteToConsoleAndLog(fmt::format(
			    "[DS_QUEUE_CAPTURE] frame={} queue={} shader=0x{:016x} covered={} colored={} coords={},{}/{},{}/{},{}/{},{}/{},{}\n",
			    frame, queue, capture.shader, covered, colored, p[0].first, p[0].second, p[1].first, p[1].second,
			    p[2].first, p[2].second, p[3].first, p[3].second, p[4].first, p[4].second));
			RebindBuffers(bindings);
			SaveDsQueueBindings(m_context, bindings, capture, queue);
			ProbeDsQueueImages(m_context, bindings, capture, queue, true);
			break;
		}
	}
	const bool light_math_sample = queue_capture && program.shader_hash==DemonsLightMathProbe::Shader &&
	    program.ds_light_math_variant &&
	    m_ds_light_tiles[captured_queue_index].valid;
	BindDsLightMath(bindings, light_math_sample ? queue_capture : nullptr);
	auto* post = BeginDsPostCompute(bindings, submit_id);
	const bool material_probe = ProbeDsMaterialLight(bindings, submit_id, true);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto args_epoch = m_context.GetBufferCache().UnregisterEpoch();
	auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	m_rebind_epoch = args_epoch;
	RebindBuffers(bindings);
	if (RebindAfterJoinEnabled()) {
		auto& cache = m_context.GetBufferCache();
		for (uint32_t pass = 0; pass < 4 && cache.UnregisterEpoch() != m_rebind_epoch; ++pass) {
			NoteRebindAfterJoin("dispatch", nullptr);
			m_rebind_epoch = cache.UnregisterEpoch();
			std::tie(args_buffer, args_offset) = cache.ObtainBuffer(
			    args_addr, sizeof(vk::DispatchIndirectCommand), false);
			EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
			RebindBuffers(bindings);
		}
	}
	vk::Buffer indirect_buffer = args_buffer->Handle();
	uint64_t   indirect_offset = args_offset;
	if (!cpu_args && IndirectDispatchClampEnabled()) {
		// Recorded before the guest bindings: the fix-up binds its own pipeline and push data.
		const auto clamped = GetIndirectDrawFixup().RecordDispatchClamp(
		    buffer, args_buffer->BufferDeviceAddress() + args_offset, limits, args_addr,
		    program.shader_hash);
		indirect_buffer = clamped.buffer;
		indirect_offset = clamped.offset;
	}
	auto coefficient_trace = BeginDsCoefficientDispatch(m_context, bindings, submit_id);
	auto count_producer_trace = BeginDsCountProducer(m_context, bindings, submit_id);
	if (count_producer_trace) BindDsCountExec(bindings,true);
    const auto relight_trace=SelectDsRelightTrace(m_context,bindings,{args.x,args.y,args.z},submit_id,
        split_on_cpu?vk::Buffer{}:indirect_buffer,indirect_offset);
    if(relight_trace.header[0])BindDsRelightTrace(bindings,relight_trace.header);
    DsVertexProducerCapture vertex_producer; // V3: no W0 capture
    DemonsPrelightTrace::ObserveCompute(m_context,bindings,submit_id,false,true,args_addr);
    if(DemonsCausalMatrix::Evidence()&&DemonsCausalMatrix::LightingQueue(program.shader_hash)!=UINT32_MAX)
        CaptureDsLightBridge(m_context,bindings,submit_id,{1,1,1},true,true);
	auto indirect_producer = BeginDsIndirectProducer(m_context, bindings, input_info, submit_id,
	    compute_program.module, {args.x, args.y, args.z}, true, args_addr,
	    split_on_cpu ? vk::Buffer{} : indirect_buffer, indirect_offset);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	const bool ds_bvh_dependency = DemonsLightingMemoryPolicy::NeedsBvhProducerBarrier(
	    DemonsBvhSrtCoherence(), program.info.bvh);
	if (has_storage_writes || ds_bvh_dependency) {
		ShaderWriteHazardBarrier(vk_buffer,
		    ds_bvh_dependency ? vk::PipelineStageFlagBits::eAllCommands
		                      : vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	// The source may be a GPU-written color/depth attachment published
	// through the overlapping image/buffer cache. Exclude UFC6 and other
	// titles from this stronger dependency.
	if (ds_gpu_zero_indirect) {
		barrier.srcAccessMask |= vk::AccessFlagBits::eColorAttachmentWrite |
		                         vk::AccessFlagBits::eDepthStencilAttachmentWrite;
	}
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	buffer.MarkGpu("misc");
	{
		const ScopedDebugLabel label(vk_buffer, "cs-indirect", program.shader_hash);
		if (split_on_cpu) {
			RecordGuestDispatch(vk_buffer, limits, {args.x, args.y, args.z}, program, pipeline);
		} else {
			vk_buffer.dispatchIndirect(indirect_buffer, indirect_offset);
		}
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::Dispatches);
	m_context.GetCommandScheduler().NoteWork();

	buffer.MarkGpu("cs-indirect", program.shader_hash);
	RetagDsRecordedComputeWrites(m_context, bindings, ds_preparation_tick);
	RememberDsBufferProducers(m_context, bindings, true, submit_id);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	if (light_math_sample) {
		vk::MemoryBarrier host_read {};
		host_read.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		host_read.dstAccessMask = vk::AccessFlagBits::eHostRead;
		vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                          vk::PipelineStageFlagBits::eHost, {}, 1, &host_read, 0, nullptr, 0, nullptr);
	}
    EndDsIndirectProducer(m_context, std::move(indirect_producer));
	// Keep the exact producer bindings alive until their after snapshot;
	// the older light-math Finish() may run deferred cache callbacks.
	if (light_math_sample) ReadDsLightMath(*queue_capture, submit_id, bindings);
	EndDsCountProducer(m_context,bindings,std::move(count_producer_trace),{args.x,args.y,args.z},
	                   split_on_cpu ? vk::Buffer {} : indirect_buffer,indirect_offset);
    SaveDsRelightTrace(m_context,bindings,submit_id,relight_trace);
	CaptureDsGeometryWriter(m_context, bindings, submit_id, {args.x, args.y, args.z},
	                           split_on_cpu ? vk::Buffer {} : indirect_buffer, indirect_offset);
	CaptureDsRelightMaskWriter(m_context, bindings, submit_id, {args.x, args.y, args.z},
	                           split_on_cpu ? vk::Buffer {} : indirect_buffer, indirect_offset);
	CaptureDsRadianceComputeWriter(m_context, bindings, submit_id, {args.x, args.y, args.z},
	                               split_on_cpu ? vk::Buffer {} : indirect_buffer, indirect_offset);
	CaptureDsProbeSourceWrite(m_context, bindings, submit_id, {args.x, args.y, args.z},
	                         split_on_cpu ? vk::Buffer {} : indirect_buffer, indirect_offset);
	if (DemonsPcDppPolicy::CaptureProducer(DsTraceEnabled(), program.shader_hash) &&
	    Config::GetShaderDumpFolder().filename() == "ds-lighting-dumps") {
		std::array<uint32_t, 3> ray_groups {args.x, args.y, args.z};
		const bool ray_valid = split_on_cpu ||
		    ReadDsGpuWords(m_context, indirect_buffer, indirect_offset, ray_groups);
		CaptureDsRayHitProducer(m_context, bindings, submit_id, ray_groups, ray_valid);
	}
	EndDsCoefficientDispatch(m_context, bindings, std::move(coefficient_trace), {args.x, args.y, args.z},
	                         split_on_cpu ? vk::Buffer {} : indirect_buffer, indirect_offset);
	if (post) ProbeDsPostImages(bindings, *post, submit_id, program.shader_hash, true, false);
	if (material_probe) {
		std::array<uint32_t, 3> groups {args.x, args.y, args.z};
		const bool valid = split_on_cpu ||
		    ReadDsGpuWords(m_context, indirect_buffer, indirect_offset, groups);
		ProbeDsMaterialLight(bindings, submit_id, false, groups, valid);
	}
	ProbeDsComputeWrites(m_context, input_info, bindings);
	if (queue_capture) {
		const auto captured_queue = DemonsQueueProbe::Queues[captured_queue_index];
		const auto& tiles = m_ds_light_tiles[captured_queue_index];
		std::array<uint32_t, 3> groups {args.x, args.y, args.z};
		const bool read = split_on_cpu ||
		    ReadDsGpuWords(m_context, indirect_buffer, indirect_offset, groups);
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DS_QUEUE_EXECUTED] frame={} queue={} shader=0x{:016x} source={} valid={} "
		    "x={} y={} z={} tile_count={}\n", frame, captured_queue, program.shader_hash,
		    split_on_cpu ? "cpu_split" : "vulkan_indirect", read, groups[0], groups[1], groups[2],
		    tiles.end - tiles.first));
		ProbeDsQueueImages(m_context, bindings, *queue_capture, captured_queue, false);
		queue_capture->light_output_sampled = read && groups[0] != 0 && groups[1] != 0 && groups[2] != 0;
	}
	CaptureDsLightTiles(bindings);
	ProbeDsLightingBuffers(m_context, bindings);
	ProbeDsAtlasInputs(m_context, bindings, submit_id, {args.x, args.y, args.z},
	                   split_on_cpu ? vk::Buffer {} : indirect_buffer, indirect_offset);
	if (DemonsCausalMatrix::Evidence()&&DemonsCausalMatrix::LightingQueue(program.shader_hash)!=UINT32_MAX) {
		std::array<uint32_t, 3> resolved_groups {args.x, args.y, args.z};
		const bool valid_groups = split_on_cpu ||
		    ReadDsGpuWords(m_context, indirect_buffer, indirect_offset, resolved_groups);
		CaptureDsLightBridge(m_context, bindings, submit_id, resolved_groups, valid_groups);
	}
	if (program.info.bda_writes) {
		m_context.GetBufferCache().SettleBdaWrites(program.shader_hash);
	}
	EndDsVertexProducer(m_context,std::move(vertex_producer));
	DemonsPrelightTrace::ObserveCompute(m_context,bindings,submit_id,true,true,args_addr);
	ResetBindings();
	if (two_pass) {
		DispatchRetracePass(buffer, cs_regs, pass2_input, args_addr, *retrace);
	}
}

// rt_half=2 pass 2: the same kernel as the re-trace variant, one 8x8 group per compact-list entry
// that pass 1 wrote (indirect arguments at the start of the re-trace buffer), with the guest's
// bindings of this dispatch.
void RenderExecutor::DispatchRetracePass(CommandBuffer& buffer, const HW::ComputeShaderInfo& cs_regs,
                                         ShaderComputeInputInfo input_info, uint64_t args_addr, Buffer& retrace) {
	const auto vk_buffer = buffer.Handle();
	vk::MemoryBarrier listed {};
	listed.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	listed.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead |
	                       vk::AccessFlagBits::eShaderWrite;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                          vk::PipelineStageFlagBits::eDrawIndirect | vk::PipelineStageFlagBits::eComputeShader, {}, 1,
	                          &listed, 0, nullptr, 0, nullptr);
	input_info.rt_fill_live    = HalfRateTiles::kFillRetracePass;
	const auto compute_program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info);
	if (input_info.stage.program == nullptr) {
		return;
	}
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	const uint64_t ds_preparation_tick = m_context.GetCommandScheduler().CurrentTick();
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	if (input_info.stage.program->info.uses_dma)
		PrefaultDmaPointers(m_context.GetBufferCache(), input_info.stage);
	FindBuffers(std::span {&descriptor_stage, 1u});

	if (input_info.stage.program->info.uses_dma) {
		m_context.PrepareBda();
	}
	if (input_info.stage.program->info.bda_writes) {
		m_context.GetBufferCache().PrepareBdaWrites();
	}
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	BindDsLightMath(bindings, nullptr);
	RebindImages(bindings);
	m_rebind_epoch = m_context.GetBufferCache().UnregisterEpoch();
	RebindBuffers(bindings);
	if (RebindAfterJoinEnabled()) {
		auto& cache = m_context.GetBufferCache();
		for (uint32_t pass = 0; pass < 4 && cache.UnregisterEpoch() != m_rebind_epoch; ++pass) {
			NoteRebindAfterJoin("dispatch", nullptr);
			m_rebind_epoch = cache.UnregisterEpoch();
			RebindBuffers(bindings);
		}
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline, std::span {&descriptor_stage, 1u});
	ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	{
		const ScopedDebugLabel label(vk_buffer, "cs-retrace", input_info.stage.program->shader_hash);
		vk_buffer.dispatchIndirect(retrace.Handle(), 0);
	}
	Common::PerfStats::Count(Common::PerfStats::Counter::Dispatches);

	buffer.MarkGpu("cs-retrace", input_info.stage.program->shader_hash);
	RetagDsRecordedComputeWrites(m_context, bindings, ds_preparation_tick);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	ProbeDsComputeWrites(m_context, input_info, bindings);
	CaptureDsLightTiles(bindings);
	ProbeDsLightingBuffers(m_context, bindings);
	if (input_info.stage.program->info.bda_writes) {
		m_context.GetBufferCache().SettleBdaWrites(input_info.stage.program->shader_hash);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
