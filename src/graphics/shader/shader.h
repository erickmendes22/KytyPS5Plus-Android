#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/shaderBindings.h"
#include "loader/guestAbi.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

namespace ShaderError {

[[nodiscard]] inline bool Fail(std::string* error, std::string_view message) {
	if (error != nullptr) {
		error->assign(message.data(), message.size());
	}
	return false;
}

} // namespace ShaderError

namespace HW {
struct VertexShaderInfo;
struct PixelShaderInfo;
struct ComputeShaderInfo;
struct ShaderRegisters;
} // namespace HW

enum class ShaderType {
	Unknown,
	Vertex,
	Pixel,
	Fetch,
	Compute,
	Mesh,
	Local,
	TessellationControl,
	TessellationEvaluation
};

namespace ShaderRecompiler::IR {
struct CompiledShaderInfo;
} // namespace ShaderRecompiler::IR

struct ShaderStageRuntime {
	const ShaderRecompiler::IR::CompiledShaderInfo* program = nullptr;
	const ShaderRecompiler::IR::ResourceSnapshot*   resources = nullptr;

	[[nodiscard]] explicit operator bool() const {
		return program != nullptr && resources != nullptr;
	}
};

constexpr uint32_t DstSel(uint32_t x, uint32_t y = 0, uint32_t z = 0, uint32_t w = 0) {
	return x | (y << 3u) | (z << 6u) | (w << 9u);
}

inline uint8_t GetDstSel(uint32_t swizzle, uint32_t channel) {
	return (swizzle >> (channel * 3u)) & 0x7u;
}

[[nodiscard]] inline bool IsValidImageSwizzle(uint32_t swizzle,
                                             uint32_t component_count = 4) noexcept {
	if ((swizzle & ~0xfffu) != 0) {
		return false;
	}
	for (uint32_t channel = 0; channel < 4; channel++) {
		const auto selector = GetDstSel(swizzle, channel);
		if (selector > 1 && (selector < 4 || selector >= 4 + component_count)) return false;
	}
	return true;
}

struct ShaderClipSpaceTransform {
	float scale[2]       = {};
	float offset[2]      = {};
	float half_extent[2] = {};
	bool  enabled        = false;
};

struct ShaderWorkgroupInputInfo {
	uint32_t threads_num[3]      = {0, 0, 0};
	uint32_t lds_size_dwords     = 0;
	uint32_t scratch_size_dwords = 0;
	uint32_t host_subgroup_size  = 64;
	uint32_t wave_size           = 64;
};

struct ShaderMeshInputInfo: ShaderWorkgroupInputInfo {
	uint32_t input_primitive      = 0;
	uint32_t primitives_per_group = 0;
	uint32_t vertices_per_group   = 0;
	uint32_t max_vertices         = 0;
	uint32_t max_primitives       = 0;
	uint32_t provoking_vertex     = 0;
	bool     fast_launch          = false;
	// Run this mesh stage through the compute emulation even when the host has
	// VK_EXT_mesh_shader (per-pipeline choice, see PipelineCache::GetGraphicsPrograms).
	bool     emulated             = false;

	[[nodiscard]] constexpr uint32_t InputPrimitiveSize() const {
		switch (static_cast<Prospero::PrimitiveType>(input_primitive)) {
			case Prospero::PrimitiveType::kPointList: return 1u;
			case Prospero::PrimitiveType::kLineList: return 2u;
			default: return 3u;
		}
	}
	[[nodiscard]] constexpr uint32_t InputPrimitiveStep() const {
		switch (static_cast<Prospero::PrimitiveType>(input_primitive)) {
			case Prospero::PrimitiveType::kTriFan:
			case Prospero::PrimitiveType::kTriStrip: return 1u;
			default: return InputPrimitiveSize();
		}
	}
	[[nodiscard]] constexpr uint32_t InputPrimitiveCount(uint32_t vertices) const {
		const auto size = InputPrimitiveSize();
		return vertices < size ? 0u : (vertices - size) / InputPrimitiveStep() + 1u;
	}
	[[nodiscard]] constexpr uint32_t InputVertexCount(uint32_t primitives) const {
		return primitives == 0u ? 0u : (primitives - 1u) * InputPrimitiveStep() + InputPrimitiveSize();
	}
};

struct ShaderTessellationInputInfo {
	uint32_t input_control_points  = 0;
	uint32_t output_control_points = 0;
	uint32_t ls_stride             = 0;
	uint32_t hs_stride             = 0;
	uint32_t domain                = 0;
	uint32_t partitioning          = 0;
	uint32_t output_topology       = 0;
};

// --gpu-indirect: a draw whose arguments stay in GPU memory. The hardware vertex/instance ID
// VGPRs exclude the draw's base vertex and base instance, which the command processor writes into
// the user SGPRs the draw packet names; here the shader derives all of them from the Vulkan draw
// parameters instead (gl_VertexIndex - gl_BaseVertex, gl_BaseVertex, ...).
struct ShaderIndirectDrawInfo {
	bool   enabled       = false;
	int8_t vertex_sgpr   = -1; // absolute SGPR receiving the base vertex, -1 if none
	int8_t instance_sgpr = -1; // absolute SGPR receiving the base instance, -1 if none
};

// Fields a vertex shader also writes, unpacked from its parameter exports, for a pixel shader
// whose manual per-vertex interpolation became hardware interpolation (IR::PackedVarying, see
// recompiler/ir/passes/ManualInterpolation.h). Filled per pipeline from the pixel shader.
struct ShaderPackedVaryingRequest {
	static constexpr uint32_t MaxEntries = 32;
	struct Entry {
		uint8_t source_location  = 0;
		uint8_t source_component = 0;
		uint8_t bit_offset       = 0;
		uint8_t bit_count        = 32;
		uint8_t format           = 0; // IR::PackedVaryingFormat
		uint8_t location         = 0;
		uint8_t component        = 0;
		uint8_t flat             = 0; // 1: the raw dword, bit-exact, in a flat uvec4 location
	};
	uint32_t count = 0;
	Entry    entries[MaxEntries] {};
};

struct ShaderVertexInputInfo {
	static constexpr int RES_MAX = 32;

	ShaderBufferResource    resources[RES_MAX];
	ShaderVertexDestination resources_dst[RES_MAX];
	ShaderVertexInputBuffer buffers[RES_MAX];
	ShaderStageRuntime      stage;
	ShaderType                  logical_stage        = ShaderType::Vertex;
	int                     resources_num       = 0;
	int                     fetch_attrib_reg    = 0;
	int                     fetch_buffer_reg    = 0;
	int                     buffers_num         = 0;
	uint32_t                wave_size           = 64;
	uint32_t                scratch_size_dwords = 0;
	uint32_t                pa_cl_vs_out_cntl    = 0;
	ShaderClipSpaceTransform clip_space;
	ShaderMeshInputInfo      mesh;
	ShaderTessellationInputInfo tess;
	bool                    fetch_external      = false;
	bool                    fetch_embedded      = false;
	ShaderIndirectDrawInfo  indirect;
	ShaderPackedVaryingRequest packed_varyings;
};

struct ShaderComputeInputInfo: ShaderWorkgroupInputInfo {
	bool               async_compute              = false;
	uint8_t            float_mode                 = 0xc0;
	uint32_t           dispatch_threads_num[3]    = {0, 0, 0};
	uint32_t           workgroup_counts[3]        = {0, 0, 0};
	uint64_t           indirect_args_address       = 0;
	bool               group_id[3]                = {false, false, false};
	bool               dispatch_thread_dimensions = false;
	bool               lds_storage                = false;
	int                thread_ids_num             = 0;
	int                workgroup_register         = 0;
	bool               tg_size_en                 = false;
	// KYTY_CS_PACK: the renderer asks for two guest workgroups of at most 32 threads (one guest
	// wave32 each) per 64-lane host workgroup; 2 = requested, else 0. The program's
	// ShaderInfo::group_pack says whether the recompiler did it.
	uint8_t            pack_groups                = 0;
	ShaderStageRuntime stage;
};

enum class ShaderAlphaBlendSource : uint8_t { None, SourceAlpha, SourceAlphaOne, SourceAlphaZero };
enum class ShaderPixelParameterMode : uint8_t { FirstVertex, LastVertex, Rectangle };

struct ShaderPixelInputInfo {
	uint32_t                                       interpolator_settings[32]    = {0};
	uint32_t                                       input_num                    = 0;
	uint32_t                                       wave_size                    = 64;
	uint32_t                                       ps_system_input_base         = 0;
	uint32_t                                       custom_interpolation_mask    = 0;
	uint32_t                                       ps_perspective_center_vgpr   = UINT32_MAX;
	uint32_t                                       ps_perspective_sample_vgpr   = UINT32_MAX;
	uint32_t                                       ps_perspective_centroid_vgpr = UINT32_MAX;
	uint8_t                                        target_output_mode[8]        = {};
	uint32_t                                       target_shader_mask           = UINT32_MAX;
	std::array<Prospero::ColorComponentMapping, 8> target_export_mapping        = {};
	uint8_t                                        target_packed_unorm_mask      = 0;
	// CB round mode per converted export: zero adds half an LSB, one truncates.
	uint8_t                                        target_round_mode            = 0;
	uint32_t                                       scratch_size_dwords          = 0;
	bool                                           ps_pos_x                     = false;
	bool                                           ps_pos_y                     = false;
	bool                                           ps_pos_z                     = false;
	bool                                           ps_pos_w                     = false;
	bool                                           ps_front_face                = false;
	bool                                           ps_front_face_all_bits       = false;
	bool                                           ps_ancillary                 = false;
	bool                                           ps_no_perspective            = false;
	ShaderPixelParameterMode                       parameter_mode = ShaderPixelParameterMode::FirstVertex;
	bool                                           ps_pixel_kill_enable         = false;
	bool                                           ps_depth_export_enable       = false;
	bool                                           ps_sample_mask_export_enable = false;
	bool                                           ps_sample_shading            = false;
	bool                                           dual_source_blending         = false;
	bool                                           object_primitive_id          = false;
	// Export logical alpha or per-channel source factors through MRT1 after channel swizzling.
	ShaderAlphaBlendSource                         alpha_blend_source = ShaderAlphaBlendSource::None;
	bool                                           ps_early_z                   = false;
	bool                                           ps_execute_on_noop           = false;
	// The previous stage is a plain vertex shader that can supply unpacked parameter fields
	// (ShaderDeviceFeatures::packed_varying_locations).
	bool                                           packed_varyings_allowed      = false;
	ShaderStageRuntime                             stage;

	bool HasPositionInput() const { return ps_pos_x || ps_pos_y || ps_pos_z || ps_pos_w; }
};

union ShaderStageInputInfo {
	const ShaderVertexInputInfo*  vertex;
	const ShaderPixelInputInfo*   pixel;
	const ShaderComputeInputInfo* compute = nullptr;
};

// Device capabilities that change which SPIR-V the recompiler may emit. Defaults are the most
// restrictive. The emitter rejects a shader that needs an unsupported feature instead of
// producing SPIR-V the device cannot accept.
// Optional device capabilities the SPIR-V backend may use. Defaults describe a desktop GPU with
// everything available (the pre-mobile behaviour); the renderer overrides them from the device.
struct ShaderDeviceFeatures {
	bool fragment_shader_barycentric      = true; // BaryCoord inputs and PerVertexKHR reads
	// Varying locations a generated pass-through geometry stage can carry between the last
	// pre-rasterization stage and the pixel shader: the smallest of the vertex/tessellation
	// output, geometry input/output and fragment input limits, in vec4 locations. Without
	// fragment_shader_barycentric, a non-zero value lets the pixel shader read barycentrics and
	// per-vertex parameters that such a geometry stage supplies. 0 means no geometryShader.
	uint32_t geometry_varying_locations   = 32;
	bool shader_float64                   = true; // Float64 arithmetic
	bool shader_shared_int64_atomics      = true; // Int64 atomics on Workgroup (LDS) memory
	bool signed_zero_inf_nan_preserve_f32 = true; // SignedZeroInfNanPreserve at width 32
	// Graphics stages whose invocations may use subgroup ballot and shuffle
	// (VkPhysicalDeviceVulkan11Properties::subgroupSupportedStages/Operations). In a stage
	// without them each invocation is emitted as a one-lane wave instead.
	bool subgroup_vertex         = true; // Vertex, Local
	bool subgroup_tessellation   = true; // TessellationControl, TessellationEvaluation
	bool subgroup_fragment       = true; // Pixel
	// Subgroup operation classes beyond ballot/shuffle (subgroupSupportedOperations). Without
	// vote, uniform-branch votes are computed from ballots; without arithmetic, wave reductions
	// keep their DPP lowering instead of becoming OpGroupNonUniform*Min.
	bool subgroup_vote           = true;
	bool subgroup_arithmetic     = true;
	// DPP quad permutations (quad_perm, row_xmask:1..3) become GroupNonUniformQuad swaps and
	// broadcasts in pixel and compute shaders instead of an indexed shuffle, which Mesa ir3
	// lowers to a loop over the distinct lane deltas (KYTY_QUAD_DPP; needs the quad subgroup
	// feature). Same results: both read the same lane of the quad. Off by default: Astro Bot
	// desert gameplay (Turnip v32, --gpu-turbo, one back-to-back pair) went from 162 to 159 GPU
	// ms per frame (fps median 5.80 -> 5.90), within run-to-run noise.
	bool subgroup_quad_dpp       = false;
	// IMAGE_BVH(64)_INTERSECT_RAY reads the node's first 64 bytes as four 16-byte loads up front
	// instead of one 4-byte load per word where it is used (KYTY_BVH_VEC=1, opt-in). ir3 on the
	// ray-traced compute pass: 18940 -> 18180 instructions, 7318 -> 6915 NOPs; the gameplay gain
	// is unmeasured and runs with it showed black streaks not yet ruled out as its doing.
	bool bvh_vector_loads        = false;
	// IMAGE_BVH(64)_INTERSECT_RAY emulation variants with the same results (KYTY_BVH_OPT bit mask,
	// ShaderBvhEmulation bits): triangle corners picked from the node's words with constant word
	// indices (1), one FP16/FP32 branch around the four box children instead of one per child
	// (2), the ray origin's NaN/Inf check as one ordered |x| < inf compare per component (4).
	// Opt-in: on Astro Bot's RT pass 6856320a (KYTY_BVH_OPT=7) fewer NOPs and SY stalls but more
	// instructions, the same 48 registers, and no faster dispatch in the one gameplay pair.
	uint32_t bvh_emulation = 0;
	// Without fragment_shader_barycentric, a pixel shader that interpolates packed parameters by
	// hand from per-vertex reads may instead read hardware-interpolated fields that its vertex
	// shader unpacks (KYTY_PACKED_INTERP; recompiler/ir/passes/ManualInterpolation.h). Locations
	// [0, value) are usable between the vertex and fragment stages; 0 is off.
	uint32_t packed_varying_locations = 0;
	// Values a register only keeps in lanes a write skipped (the kept operand of an exec-merge
	// select or a DPP update) are taken as dead: a raw I/J kept by a DPP update no longer needs
	// the geometry stage, and packed-varying lowering zeroes such kept vertex data so the
	// per-vertex reads behind it go away (KYTY_DROP_CARRIED; default Mesa Turnip). Astro Bot's
	// particle pixel shaders (5a10a907, 969c5ed1) then run without the geometry stage, which drew
	// them magenta.
	bool drop_carried_vertex_data = false;
	// Before packed-varying lowering, a select whose operand is a select on the same condition
	// takes that select's operand on its own side (passes/ManualInterpolation.h,
	// FoldSameConditionSelects; same results). Exec-masked blocks that reuse a register holding a
	// raw vertex dword otherwise keep the per-vertex read alive through the nested select of the
	// next masked write, and the pixel shader keeps its geometry stage (KYTY_SELECT_FOLD; default
	// Mesa Turnip).
	bool fold_same_condition_selects = false;
	// Without fragment_shader_barycentric, a pixel parameter slot that a flat and a smooth input
	// alias (SPI_PS_INPUT_CNTL offsets) stays a native input while the shader uses only one of the
	// two modes; that alias alone is declared, with its own qualifier. Otherwise any use of such a
	// slot needs per-vertex data (geometry stage), and packed-varying lowering that leaves only the
	// flat alias in use is rejected (KYTY_ALIAS_SLOTS; default Mesa Turnip).
	bool single_mode_alias_slots = false;
	// shaderStorageImageArrayNonUniformIndexing. Without it, GPU-selected storage images are
	// indexed dynamically without the NonUniform decoration (shaderStorageImageArrayDynamicIndexing).
	bool storage_image_array_non_uniform_indexing = true;
	// VK_EXT_mesh_shader. Without it a Mesh stage is emitted as a compute shader that writes its
	// vertices and triangles to buffers a generated vertex shader reads back (mesh emulation).
	bool mesh_shader = true;
	// V_MIN/V_MAX/V_MED3_F32 use GLSL NMin/NMax plus a signed-zero fix-up (equal operands
	// combine their bits) instead of classifying both operands in integer code. Same results
	// except the payload when both operands are NaN (KYTY_FAST_FMINMAX; default Mesa Turnip).
	bool native_float_min_max = false;
	// robustBufferAccess2 returns zero for out-of-range storage buffer reads, which is what the
	// per-load bounds branch produces: 32-bit buffer loads skip it (KYTY_ROBUST_LOADS; default
	// Mesa Turnip, needs robustBufferAccess2 with a 4-byte access alignment).
	bool robust_buffer_loads = false;
	// Read-only scalar buffers (S_BUFFER_LOAD only, never written or GLC-loaded by the shader)
	// whose descriptor size is at most this many bytes are bound as uniform buffers and read
	// with UBO loads, which Mesa ir3 can hoist into its preamble / constant file
	// (KYTY_UBO_CONSTANTS=1; off by default: mixed per-shader timings on Turnip v32 and ~28 % more
	// shader permutations, since the 16-byte base alignment it specializes on varies per draw).
	// Out-of-range reads rely on robustBufferAccess2 for uniform buffers. 0: storage buffers only.
	uint32_t uniform_constant_buffer_bytes = 0;
	// At most this many uniform buffers per shader (maxPerStageDescriptorUniformBuffers margin).
	uint32_t uniform_buffers_per_stage = 0;
	// V_ADD/SUB/SUBREV/MUL_F16 and the V_PK_ADD/MUL_F16 lanes become native 16-bit float SPIR-V
	// (Float16, RTE, denormals and signed zero/inf/NaN preserved at width 16) instead of F32 math
	// rounded to F16; identical results except NaN payloads (KYTY_NATIVE_FP16; needs
	// shaderFloat16 and the matching float controls).
	bool native_float16 = false;
	// S_CBRANCH_EXECZ-style vote branches around lane-local regions become per-lane branches
	// on the EXEC predicate (Spirv::ConvertExecVoteBranches; KYTY_LANE_BRANCHES=1). Experimental,
	// off: the Astro Bot intro's particle swarm is not confirmed to render with it.
	bool lane_exec_branches = false;
	// Pixel-shader prologues save EXEC, apply S_WQM and restore it as a lane mask; the lane's own
	// bit of WQM(ballot(true)) is always set, so its predicate folds to true instead of a select
	// per VALU write (KYTY_WQM_LANE_FOLD=1). Experimental, off (see lane_exec_branches).
	bool wqm_entry_lane_fold = false;
	// A wave32 (wave64) program on a wider host subgroup (Adreno: 64/128 lanes) sees its own
	// 32 (64) lanes: IR ballots return the wave's words, LaneId / READLANE / WRITELANE /
	// READFIRSTLANE / DS_APPEND use wave-local lane numbers (KYTY_WAVE_SLICES=1). Without it,
	// host lanes >= 32 read lane (n & 31) of the first wave's mask. Experimental, off.
	bool wave_slices = false;
	// V_READLANE_B32 and V_READFIRSTLANE_B32 read their wave-uniform lane with
	// OpGroupNonUniformBroadcast (a constant id for immediate lanes, else a dynamically uniform one,
	// SPIR-V 1.5) instead of OpGroupNonUniformShuffle, and gl_SubgroupSize becomes a constant in
	// compute programs that run with a required subgroup size (KYTY_LANE_BROADCAST). Mesa ir3 on
	// a7xx/a8xx rewrites every shuffle, a uniform index included, as a rotate by
	// (index - invocation), which is divergent and becomes a loop over the distinct values; a
	// broadcast is one movs. Same lane read, same result.
	bool constant_lane_broadcast = false;
	// The value V_READFIRSTLANE_B32 returns is one of its source's values in a lane where its EXEC
	// predicate holds, so selects on a condition that predicate implies have one possible value
	// (KYTY_FIRSTLANE_SETS). A waterfall loop over a per-lane register index (readfirstlane, M0,
	// V_MOVRELS) then compares M0 only with the few indices that lane can hold instead of every
	// register after the base: Astro Bot's VS 7d308cde had 24 such chains of ~110 selects each.
	// Off by default: with the round-9 defaults its pass 32d3ce28 costs 1.4 ms/frame without the
	// knob and 1.0 with it (VS 10247 -> 7722 ir3 instructions), screenshots match, but only one
	// device run with it completed and fps did not rise (the frame is CPU-bound).
	bool first_lane_value_sets = false;
	// Smallest host subgroup size of any stage (minSubgroupSize, or subgroupSize without
	// subgroup size control); 0: unknown. A guest lane below it never wraps.
	uint32_t subgroup_size_min = 0;
	// Compute programs whose wave size lies in [min, max] run with it as the required subgroup
	// size (subgroupSizeControl for compute); 0: no size control.
	uint32_t compute_required_subgroup_min = 0;
	uint32_t compute_required_subgroup_max = 0;
	// --render-scale is active: pixel and compute shaders read the rescale push dwords
	// (IR::PushData::Rescale*) to unscale gl_FragCoord and scale texel coordinates. Off, the
	// emitted SPIR-V is unchanged.
	bool render_scale = false;
	// Diagnostics (--render-scale-flags): leave gl_FragCoord / texel coordinates untouched.
	bool render_scale_keep_fragcoord = false;
	bool render_scale_keep_texels    = false;
};

// ShaderDeviceFeatures::bvh_emulation bits.
enum ShaderBvhEmulation : uint32_t {
	ShaderBvhTriangleWords = 1u,
	ShaderBvhBoxBranch     = 2u,
	ShaderBvhOriginCheck   = 4u,
};

// True when a shader of this stage may emit native subgroup operations on the device.
[[nodiscard]] constexpr bool ShaderSubgroupOpsAvailable(const ShaderDeviceFeatures& features,
                                                        ShaderType                  stage) {
	switch (stage) {
		case ShaderType::Vertex:
		case ShaderType::Local: return features.subgroup_vertex;
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation: return features.subgroup_tessellation;
		case ShaderType::Pixel: return features.subgroup_fragment;
		default: return true;
	}
}

inline const ShaderWorkgroupInputInfo* ShaderWorkgroupInput(ShaderType           stage,
                                                            ShaderStageInputInfo input) {
	switch (stage) {
		case ShaderType::Compute: return input.compute;
		case ShaderType::Mesh: return &input.vertex->mesh;
		default: return nullptr;
	}
}

uint32_t ShaderPixelParameterMappedLocation(const ShaderPixelInputInfo& info, uint32_t input);
uint32_t ShaderPixelParameterLocation(const ShaderPixelInputInfo& info,
                                      std::span<const uint32_t> active_inputs, uint32_t input);
bool     ShaderPixelParameterIsFlat(const ShaderPixelInputInfo& info, uint32_t input);
bool     ShaderPixelParameterIsCustom(const ShaderPixelInputInfo& info, uint32_t input);

struct ShaderSharp {
	uint16_t offset_dw : 15;
	uint16_t size      : 1;
};

struct ShaderUserData {
	uint16_t*    direct_resource_offset;
	ShaderSharp* sharp_resource_offset[4];
	uint16_t     eud_size_dw;
	uint16_t     srt_size_dw;
	uint16_t     direct_resource_count;
	uint16_t     sharp_resource_count[4];
};

struct ShaderRegisterRange {
	uint16_t start;
	uint16_t end;
};

struct ShaderDrawModifier {
	uint32_t enbl_start_vertex_offset   : 1;
	uint32_t enbl_start_index_offset    : 1;
	uint32_t enbl_start_instance_offset : 1;
	uint32_t enbl_draw_index            : 1;
	uint32_t enbl_user_vgprs            : 1;
	uint32_t render_target_slice_offset : 3;
	uint32_t fuse_draws                 : 1;
	uint32_t compiler_flags             : 23;
	uint32_t is_default                 : 1;
	uint32_t reserved                   : 31;
};

struct ShaderRegister {
	uint32_t offset;
	uint32_t value;
};

} // namespace Libs::Graphics

namespace Loader::GuestAbi {
// Eight bytes of integers: one INTEGER eightbyte.
template <>
struct HleAggregate<Libs::Graphics::ShaderRegister> {
	static constexpr std::array<ArgClass, 1> kEightbytes {ArgClass::Integer};
};
} // namespace Loader::GuestAbi

namespace Libs::Graphics {

struct ShaderSpecialRegs {
	ShaderRegister      ge_cntl;
	ShaderRegister      vgt_shader_stages_en;
	uint32_t            dispatch_modifier;
	ShaderRegisterRange user_data_range;
	ShaderDrawModifier  draw_modifier;
	ShaderRegister      vgt_gs_out_prim_type;
	ShaderRegister      ge_user_vgpr_en;
};

struct ShaderSemantic {
	uint32_t semantic         : 8;
	uint32_t hardware_mapping : 8;
	uint32_t size_in_elements : 4;
	uint32_t is_f16           : 2;
	uint32_t is_flat_shaded   : 1;
	uint32_t is_linear        : 1;
	uint32_t is_custom        : 1;
	uint32_t static_vb_index  : 1;
	uint32_t static_attribute : 1;
	uint32_t reserved         : 1;
	uint32_t default_value    : 2;
	uint32_t default_value_hi : 2;
};

struct Shader {
	uint32_t             file_header;
	uint32_t             version;
	ShaderUserData*      user_data;
	const volatile void* code;
	ShaderRegister*      cx_registers;
	ShaderRegister*      sh_registers;
	ShaderSpecialRegs*   specials;
	ShaderSemantic*      input_semantics;
	ShaderSemantic*      output_semantics;
	uint32_t             header_size;
	uint32_t             shader_size;
	uint32_t             embedded_constant_buffer_size_dqw;
	uint32_t             target;
	uint32_t             num_input_semantics;
	uint16_t             scratch_size_dw_per_thread;
	uint16_t             num_output_semantics;
	uint16_t             special_sizes_bytes;
	uint8_t              type;
	uint8_t              num_cx_registers;
	uint8_t              num_sh_registers;
};

struct ShaderMappedData {
	Prospero::ShaderBinaryType type {};
	ShaderUserData* user_data           = nullptr;
	ShaderSemantic* input_semantics     = nullptr;
	uint32_t        num_input_semantics = 0;
	uint32_t        code_size_bytes     = 0;
	uint32_t        scratch_size_dwords = 0;
};

void ShaderInit();
void ShaderMapUserData(uint64_t addr, const ShaderMappedData& data);
uint32_t ShaderPixelExportTarget(uint32_t shader_mask, uint32_t export_index);

void     ShaderDbgDumpInputInfo(const ShaderVertexInputInfo& info);
void     ShaderDbgDumpInputInfo(const ShaderPixelInputInfo& info);
void     ShaderDbgDumpInputInfo(const ShaderComputeInputInfo& info);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_ */
