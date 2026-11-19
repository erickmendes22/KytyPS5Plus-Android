#ifndef GRAPHICS_HOST_GPU_RENDERER_RENDER_INDIRECT_H
#define GRAPHICS_HOST_GPU_RENDERER_RENDER_INDIRECT_H

// --gpu-indirect: indirect draws whose arguments stay in GPU memory. Design follows shadPS4
// (GPL-2.0-or-later, Copyright 2024 shadPS4 Emulator Project): Rasterizer::DrawIndirect binds the
// guest argument buffer for vkCmdDraw(Indexed)Indirect and the vertex shader prologue takes the
// base vertex/instance user SGPRs from gl_BaseVertex/gl_BaseInstance. Kyty adds a compute pass
// (shaders/indirect_draw_fixup.comp) that clamps indexed records to INDEX_BUFFER_SIZE, applies a
// GPU-written draw count, and derives the dispatch and draw of emulated mesh stages.

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace Libs::Graphics {

class Buffer;
class CommandBuffer;
class RenderContext;
struct IndirectDrawArgs;

// Guest memory an indirect draw's fix-up pass reads, resolved while the draw's resources are
// acquired.
struct IndirectDrawSources {
	vk::DeviceAddress args  = 0;
	vk::DeviceAddress count = 0;
};

class IndirectDrawFixup {
public:
	// Host records for the draw: VkDraw(Indexed)IndirectCommand, tightly packed.
	struct Records {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0;
		uint32_t   stride = 0;
		uint32_t   count  = 0;
	};
	using Sources = IndirectDrawSources;
	// Emulated mesh draw: the input assembly of the mesh stage and the workgroup slots reserved
	// for its outputs.
	struct MeshShape {
		uint32_t primitive_size       = 0;
		uint32_t primitive_step       = 0;
		uint32_t primitives_per_group = 0;
		uint32_t indices_per_group    = 0; // triangle-list indices a workgroup slot owns
		uint32_t slots                = 0; // reserved groups x instances
	};
	// Outputs of a mesh fix-up: the compute pass's record (PushData::MeshIndirect*), its
	// dispatch command and the command of the triangle-list draw.
	struct MeshRecords {
		vk::Buffer        buffer          = nullptr;
		vk::DeviceAddress record          = 0;
		uint64_t          dispatch_offset = 0;
		uint64_t          draw_offset     = 0;
	};

	explicit IndirectDrawFixup(RenderContext& context);
	~IndirectDrawFixup();
	KYTY_CLASS_NO_COPY(IndirectDrawFixup);

	// Obtains the guest records (and count) from the buffer cache. Call while resources are
	// acquired, before anything of the draw is recorded.
	[[nodiscard]] Sources AcquireSources(const IndirectDrawArgs& args);
	// Records the fix-up dispatch on the primary command buffer, ahead of the render pass the
	// draw lands in (ending it first when passes are not deferred).
	[[nodiscard]] Records Record(CommandBuffer& buffer, const IndirectDrawArgs& args,
	                             const Sources& sources);
	// The same for an emulated mesh draw (one indexed record).
	[[nodiscard]] MeshRecords RecordMesh(CommandBuffer& buffer, const IndirectDrawArgs& args,
	                                     const Sources& sources, const MeshShape& shape);
	// A guest VkDispatchIndirectCommand the GPU wrote, clamped to the host workgroup limits
	// (maxComputeWorkGroupCount) into a host record; a clamp drops work and is logged once the
	// feedback arrives. `site` is the guest record address (for the log).
	struct DispatchRecord {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0;
	};
	[[nodiscard]] DispatchRecord RecordDispatchClamp(CommandBuffer& buffer, vk::DeviceAddress args,
	                                                 const std::array<uint32_t, 3>& limits,
	                                                 uint64_t site);
	// Records the draw itself inside the open render pass.
	void Draw(vk::CommandBuffer pass, const Records& records, bool indexed) const;

	// Workgroup slots to reserve for an emulated mesh draw of at most `groups` groups per
	// instance: 512 instances, or twice the largest request seen (GPU feedback, a few frames
	// late), within `max_slots`. A request beyond the reservation draws fewer instances, which
	// is logged once the feedback arrives.
	[[nodiscard]] uint32_t MeshSlots(uint32_t groups, uint32_t max_slots);

	// Bytes of guest records the packet spans (0 if it cannot be read as one range).
	[[nodiscard]] static uint64_t GuestRecordsSize(const IndirectDrawArgs& args);

	// Whether the last draw from these guest records drew nothing. Such a packet costs more on
	// the GPU path (a compute pass and an empty render pass) than reading its records on the
	// CPU, which then skips it: Astro Bot's GPU-culled particle draw is empty for a whole scene.
	// Learned from the CPU path's reads and from GPU feedback (a frame or two late). Used by
	// --gpu-indirect 2.
	[[nodiscard]] bool LikelyEmpty(uint64_t args_addr);
	void               NoteCpuResult(uint64_t args_addr, bool empty);

private:
	struct Output {
		vk::Buffer        buffer  = nullptr;
		uint64_t          offset  = 0;
		vk::DeviceAddress address = 0;
	};
	struct Feedback {
		uint64_t tick     = 0; // 0: free
		uint64_t site     = 0; // guest record address
		uint32_t reserved = 0; // mesh: reserved workgroup slots, 0 for draws
		bool     mesh     = false;
		bool     dispatch = false; // RecordDispatchClamp: the word is 1 if it clamped
	};
	static constexpr uint32_t FeedbackSlots = 64;

	[[nodiscard]] Output Dispatch(CommandBuffer& buffer, uint64_t output_bytes, void* params,
	                              uint32_t groups);
	void PollFeedback();
	// A zeroed host-visible word for the next fix-up's feedback, or 0 if none is free.
	[[nodiscard]] vk::DeviceAddress AcquireFeedback(uint32_t& slot);
	void CommitFeedback(uint32_t slot, uint64_t site, bool mesh, uint32_t reserved,
	                    bool dispatch = false);

	RenderContext&                         m_context;
	vk::PipelineLayout                     m_layout     = nullptr;
	vk::Pipeline                           m_pipeline   = nullptr;
	bool                                   m_multi_draw = false;
	std::unique_ptr<Buffer>                m_feedback_buffer;
	std::array<Feedback, FeedbackSlots>    m_feedback {};
	uint32_t                               m_feedback_next = 0;
	uint32_t                               m_mesh_slots_seen = 0;
	std::unordered_map<uint64_t, bool>     m_empty; // last result per guest record address
};

} // namespace Libs::Graphics

#endif // GRAPHICS_HOST_GPU_RENDERER_RENDER_INDIRECT_H
