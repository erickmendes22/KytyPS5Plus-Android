#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/passEnd.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct ShaderBufferResource;
struct ShaderComputeInputInfo;
struct ShaderVertexInputInfo;
struct RenderDepthInfo;
struct RenderColorInfo;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
struct DrawRenderState;
class IndirectDrawFixup;
struct IndirectDrawSources;
struct MeshDispatchSource;
class RenderContext;
class CommandScheduler;
class GpuTimer;
struct GpuPassInfo;
struct RenderExecutorTestAccess;

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	DispatchIndirect = 9,
	Unknown,
};

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

struct DrawIndexArgs {
	uint32_t         index_count                = 0;
	const void*      index_addr                 = nullptr;
	uint32_t         instance_count             = 0;
	uint32_t         index_type_and_size        = 0;
	int32_t          base_vertex                = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
};

struct DrawAutoArgs {
	uint32_t         vertex_count               = 0;
	uint32_t         instance_count             = 0;
	uint32_t         first_vertex               = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
};

// --gpu-indirect: a DRAW_(INDEX_)INDIRECT(_MULTI) packet executed with Vulkan indirect draws that
// read the guest records (VkDraw(Indexed)IndirectCommand layout) on the GPU.
struct IndirectDrawArgs {
	uint64_t               args_addr           = 0; // first record
	uint32_t               stride              = 0; // bytes between records
	uint32_t               max_count           = 0; // records, or their upper bound with count_addr
	uint64_t               count_addr          = 0; // GPU-written record count, 0 if none
	bool                   indexed             = false;
	uint32_t               index_type_and_size = 0;
	uint64_t               index_base_addr     = 0;
	uint32_t               index_buffer_size   = 0; // INDEX_BUFFER_SIZE, in indices
	ShaderIndirectDrawInfo sgprs;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};


class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	// Render passes are deferred: BeginRendering opens a secondary command buffer that collects
	// the pass's draws, and EndRendering records begin/execute/end into the primary. Commands
	// recorded through Handle() while a pass is open therefore execute BEFORE the pass; only
	// PassHandle() records inside it. This lets mesh-emulation compute dispatches run ahead of
	// the pass instead of splitting it per draw (a full tile load/store on tilers).
	void BeginRendering(const RenderState& state) const;
	void EndRendering(PassEnd reason = PassEnd::Other) const;
	[[nodiscard]] bool IsRendering() const noexcept { return m_rendering; }
	// Counts render passes; Image::pass_serial records the last pass that used an image.
	[[nodiscard]] uint64_t PassSerial() const noexcept { return m_pass_serial; }
	[[nodiscard]] uint32_t PassDraws() const noexcept { return m_pass_draws; }
	// Whether an image barrier for an image the open pass does not use may be recorded on the
	// primary buffer ahead of the pass (deferred passes and --layout-mode >= 2).
	[[nodiscard]] bool HoistsBarriers() const noexcept { return m_hoist_barriers; }
	// The effective --render-pass-mode (auto resolves per host driver).
	[[nodiscard]] static uint32_t RenderPassMode(const GraphicContext& graphics);
	// --perf-stats GPU timestamps: closes a timed region (see GpuTimer::Mark).
	void MarkGpu(const char* kind, uint64_t hash = 0) const;
	void MarkGpu(const char* kind, uint64_t hash, const GpuPassInfo& info) const;
	// Names the render pass in GPU timings after the first draw's shader; counts its draws.
	void NotePassDraw(uint64_t shader_hash, uint64_t indices) const {
		if (m_pass_shader == 0) {
			m_pass_shader = shader_hash;
		}
		m_pass_draws++;
		m_pass_indices += indices;
	}

	[[nodiscard]] vk::CommandBuffer Handle() const;
	// The secondary command buffer of the open render pass; valid only between BeginRendering
	// and the next EndRendering.
	[[nodiscard]] vk::CommandBuffer PassHandle() const;
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	void BeginPassBuffer(const RenderState& state) const;
	void RecordBeginRendering(const RenderState& state, bool secondary) const;
	[[nodiscard]] bool DefersRenderPasses() const;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	CommandScheduler&   m_scheduler;
	vk::CommandBuffer   m_buffer          = nullptr;
	mutable vk::CommandBuffer m_pass_buffer = nullptr;
	uint32_t            m_debug_op        = 0;
	uint64_t            m_debug_submit_id = 0;
	uint32_t            m_debug_arg0      = 0;
	uint32_t            m_debug_arg1      = 0;
	uint32_t            m_debug_arg2      = 0;
	uint32_t            m_debug_arg3      = 0;
	uint64_t            m_debug_arg4      = 0;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	mutable uint64_t    m_pass_shader  = 0;
	mutable uint32_t    m_pass_draws   = 0;
	mutable uint64_t    m_pass_indices = 0;
	mutable uint64_t    m_pass_serial  = 0;
	bool                m_hoist_barriers = false;
	GpuTimer&           m_timer;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;

	friend class CommandScheduler;
};

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context);
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode, bool async_compute);
	void DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
	                      uint32_t mode, bool async_compute);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void                           FindBuffers(std::span<PreparedBindings* const> stages);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	// Image layout transitions and descriptor writes (may end the open render pass), then the
	// bind/push commands on `target`. CommitBindings does both on the primary command buffer;
	// graphics draws split them around BeginRendering so the binds land in the pass.
	void PrepareBindingWrites(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                          const PipelineCache::Pipeline&     pipeline,
	                          std::span<PreparedBindings* const> bindings);
	void WriteRescalePushData(std::span<PreparedBindings* const> bindings, float render_scale);
	void RecordBindings(vk::CommandBuffer target, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline& pipeline);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings);

private:
	void DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	void DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);
	// --gpu-indirect. False, before anything was recorded or any guest memory read, when the draw
	// needs the CPU path (the reason is logged once).
	[[nodiscard]] bool DrawIndirect(uint64_t submit_id, CommandBuffer& buffer,
	                                const IndirectDrawArgs& args);
	// --gpu-indirect: see IndirectDrawFixup::LikelyEmpty.
	[[nodiscard]] bool IndirectDrawLikelyEmpty(uint64_t args_addr);
	void               NoteIndirectDrawCpuResult(uint64_t args_addr, bool empty);
	IndirectDrawFixup& GetIndirectDrawFixup();

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		std::optional<PreparedBindings> pixel;
	};

	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	// --render-scale: moves image views that a later SetScaled left on the other-size backing
	// (attachment scaling in AcquireRenderTargets, BlockScaling by another binding) to the
	// current backing, so descriptors, layout transitions and rescale data agree.
	void RefreshBindingViews(std::span<PreparedBindings* const> stages);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slice_offset, uint32_t render_target_slot,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                   const DrawEmitInfo& emit, DrawRenderState& state);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer,
	                                          const DrawCallInfo& draw,
	                                          uint32_t            render_target_slice_offset,
	                                          DrawRenderState& state);
	void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer, const DrawCallInfo& draw,
	                         DrawRenderState& state, vk::PrimitiveTopology topology,
	                         const DrawEmitInfo& emit, const DrawIndexBufferSource& index_source,
	                         bool primitive_restart_enable,
	                         const IndirectDrawArgs* indirect = nullptr);
	// Mesh emulation result: the vertex records' address for the generated vertex stage and the
	// index count of the triangle-list draw that reads them.
	struct EmulatedMeshDraw {
		uint64_t   vertex_address = 0;
		vk::Buffer index_buffer   = nullptr;
		uint64_t   index_offset   = 0;
		uint32_t   index_count    = 0;
		// --gpu-indirect: the VkDrawIndexedIndirectCommand of the triangle-list draw.
		vk::Buffer draw_buffer = nullptr;
		uint64_t   draw_offset = 0;
	};
	// Records the compute pass of an emulated mesh draw on the primary command buffer, ahead of
	// the deferred render pass, and returns its triangle indices to bind as the index buffer.
	// draw_data is the native mesh draw prefix.
	[[nodiscard]] EmulatedMeshDraw DispatchEmulatedMesh(CommandBuffer&               buffer,
	                                                    const ShaderVertexInputInfo& mesh_input,
	                                                    const ShaderProgram&         mesh_program,
	                                                    PreparedBindings&            mesh_bindings,
	                                                    std::span<const uint32_t>    draw_data,
	                                                    uint32_t groups, uint32_t instances);
	[[nodiscard]] EmulatedMeshDraw RecordEmulatedMesh(CommandBuffer&               buffer,
	                                                  const ShaderVertexInputInfo& mesh_input,
	                                                  const ShaderProgram&         mesh_program,
	                                                  PreparedBindings&            mesh_bindings,
	                                                  const MeshDispatchSource&    source);
	// --gpu-indirect: the same for an indirect draw. Its fix-up pass derives the workgroup counts
	// and the draw record from the guest record, ahead of the compute pass.
	[[nodiscard]] EmulatedMeshDraw DispatchEmulatedMeshIndirect(
	    CommandBuffer& buffer, const ShaderVertexInputInfo& mesh_input,
	    const ShaderProgram& mesh_program, PreparedBindings& mesh_bindings,
	    const IndirectDrawArgs& args, const IndirectDrawSources& sources);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer,
	                                              uint32_t render_target_slice_offset);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer,
	                                                       uint32_t render_target_slice_offset);
	// Attachments of the open render pass (set by MarkPassImages) and --perf-stats accounting of
	// attachment transitions that ended a pass with the same attachments (layout changes).
	std::array<ImageId, RENDER_COLOR_ATTACHMENTS_MAX + 1> m_pass_images {};
	uint32_t                                              m_pass_image_count = 0;
	void NoteAttachmentPassEnd(std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
	                           std::span<const VulkanImageState> old_states);
	// Records the pass the images of a draw landed in (see CommandBuffer::HoistsBarriers).
	void                      MarkPassImages(const CommandBuffer&              buffer,
	                                         std::span<PreparedBindings* const> stages,
	                                         std::span<const RenderColorInfo> colors,
	                                         const RenderDepthInfo&           depth);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	void                     UpdateComputeMetaClears(const ShaderComputeInputInfo& input,
	                                                 const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
	                                              CommandBuffer& command, uint32_t group_x,
	                                              uint32_t group_y, uint32_t group_z, uint32_t mode);

	RenderContext&                        m_context;
	GraphicsBindings                     m_graphics_bindings;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;
	// PrepareBindingWrites scratch: ranges the current draw writes (images, attachments, buffers).
	struct WrittenRange {
		uint64_t address;
		uint64_t size;
		bool     buffer;
	};
	std::vector<WrittenRange>             m_written_ranges;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	std::array<vk::DescriptorBufferInfo, 2> m_address_buffers {};
	ShaderRecompiler::IR::PushData        m_pending_push_data {};
	vk::ShaderStageFlags                  m_pending_push_stages {};
	bool                                  m_pending_push = false;
	// --render-scale: the attachments acquired for the current draw are at the scaled size
	// (AcquireRenderTargets); pixel shaders then unscale gl_FragCoord. Reset with the bindings.
	bool                                  m_pass_scaled = false;
	// --gpu-indirect argument fix-up pass, created on the first GPU indirect draw.
	std::unique_ptr<IndirectDrawFixup>    m_indirect_fixup;

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
