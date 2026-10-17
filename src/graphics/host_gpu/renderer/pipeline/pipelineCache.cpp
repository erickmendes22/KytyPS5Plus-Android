#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDiskCache.h"

#include "graphics/host_gpu/renderer/image/renderScale.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/perfStats.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/pipeline/hostShaderQuirks.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"
#include "graphics/host_gpu/renderer/pipeline/shaderDump.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_set>
#include <variant>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

// KYTY_SHADER_IR_DUMP=<hex prefixes> (KYTY_PIPELINE_STATS syntax): the guest disassembly and IR of
// the matching shaders go to the log without enabling it for every shader.
bool IrDumpRequested(uint64_t hash) {
	static const std::vector<std::string> prefixes = [] {
		const char* spec = std::getenv("KYTY_SHADER_IR_DUMP");
		return spec != nullptr ? PipelineStats::ParseSpec(spec) : std::vector<std::string> {};
	}();
	return !prefixes.empty() && PipelineStats::Matches(prefixes, hash);
}

// Android ends apps without running destructors, so it checkpoints the driver cache much more
// often (on a worker thread, see CheckpointDriverCache).
#if defined(__ANDROID__)
constexpr auto DriverCacheSaveInterval = std::chrono::seconds {10};
#else
constexpr auto DriverCacheSaveInterval = std::chrono::minutes {5};
#endif

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	// Driver cache entries already distinguish shader code and pipeline state across builds.
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n",
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv, bool maintenance8_enabled) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	spvtools::ValidatorOptions validator_options;
	validator_options.SetAllowOffsetTextureOperand(maintenance8_enabled);
	if (tools.Validate(spirv.data(), spirv.size(), validator_options)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		std::vector<uint32_t>                       function_code;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	// Translates one guest program from copies of everything the recompiler reads, so it can run
	// on a worker while the guest moves on. A job for a new source stops at the resource plan: its
	// specialization needs guest descriptor memory, which only the GPU thread may read (and only
	// at a draw that uses the shader), so the GPU thread materializes and emits at pickup (~10 ms
	// measured, against 0.3-4.6 s for the translation itself). A job for a known source carries
	// the specialization and also emits SPIR-V and the module.
	struct TranslateJob: ShaderWorkerPool::Job {
		// Inputs, owned.
		std::vector<uint32_t>            code;
		std::vector<uint32_t>            back_code;
		std::array<uint32_t, 40>         user_data {};
		ShaderVertexInputInfo            vertex_input {};
		ShaderPixelInputInfo             pixel_input {};
		ShaderComputeInputInfo           compute_input {};
		ShaderRecompiler::CompileOptions options; // spans and pointers refer to the members above
		const char*                      stage_name = nullptr;
		GraphicContext*                  graphics   = nullptr;
		// A compile request: the entry exists and this specialization is wanted.
		bool                                         compile = false;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		uint32_t                                     push_data_cursor = 0;
		// Results, valid once Done().
		bool                               needs_sync = false; // calls other code: link on the GPU thread
		ShaderRecompiler::TranslateResult  translated;
		ShaderRecompiler::IR::ResourcePlan plan;
		std::optional<Permutation>         permutation; // compile == true

		~TranslateJob() override {
			if (permutation && permutation->handle.module != nullptr) {
				ShaderDump::ForgetModule(permutation->handle.module);
				graphics->device.destroyShaderModule(permutation->handle.module, nullptr);
			}
		}

		void Run() override {
			std::optional<Common::PerfStats::ScopedStall> worker_time;
			if (ShaderWorkerPool::OnWorkerThread()) {
				worker_time.emplace(Common::PerfStats::StallKind::ShaderWorker, stage_name);
			}
			{
				const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Translate);
				auto source = ShaderRecompiler::PrepareShaderSource(code, options);
				if (source.call) {
					needs_sync = true;
					return;
				}
				translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
			}
			if (compile) {
				permutation = CompilePermutation(*graphics, stage_name, options, std::move(translated),
				                                 specialization, push_data_cursor, {});
			} else {
				const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Plan);
				plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			}
		}
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan plan,
		            std::unique_ptr<ShaderRecompiler::ShaderSource> call_source)
		    : call_source(std::move(call_source)), resource_plan(std::move(plan)) {}

		std::span<const uint32_t> FunctionCode() const {
			return call_source
			    ? std::span<const uint32_t>(call_source->linked->code).subspan(call_source->code.size())
			    : std::span<const uint32_t>{};
		}

		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			const auto revision = call_source->revision;
			ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
			resource_plan.source_reads = call_source->reads;
			return revision != call_source->revision;
		}

		template <bool HasCalls>
		Permutation* FindPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t push_data_cursor) {
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization));
			for (auto& candidate: permutations) {
				const auto& layout = candidate.program.bindings;
				if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
				        push_data_cursor, layout.ShaderDataDwords()) ||
				    candidate.specialization != specialization) {
					continue;
				}
				if constexpr (HasCalls) {
					if (!std::ranges::equal(candidate.function_code, FunctionCode())) continue;
				}
				return &candidate;
			}
			return nullptr;
		}

		// A job compiling this specialization at this push-data cursor, if one is in flight.
		std::shared_ptr<TranslateJob> FindPendingCompile(uint32_t push_data_cursor) const {
			for (const auto& job: pending) {
				if (job->push_data_cursor == push_data_cursor && job->specialization == specialization) {
					return job;
				}
			}
			return nullptr;
		}

		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		// Draws, pipeline jobs and the stage runtimes keep pointers to installed permutations:
		// a deque never moves them.
		std::deque<Permutation>                      permutations;
		std::vector<std::shared_ptr<TranslateJob>>   pending;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	// Emits SPIR-V for a translated program and creates its module. Runs on any thread: it reads
	// only its arguments and immutable device state. The handle id is assigned at install.
	static Permutation CompilePermutation(GraphicContext& graphics, const char* stage_name,
	                                      const ShaderRecompiler::CompileOptions&      options,
	                                      ShaderRecompiler::TranslateResult            translated,
	                                      ShaderRecompiler::IR::ResourceSpecialization specialization,
	                                      uint32_t                  push_data_start_dword,
	                                      std::span<const uint32_t> function_code) {
		ShaderRecompiler::CompileResult result;
		{
			const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Emit);
			result = ShaderRecompiler::CompileProgram(std::move(translated), options,
			                                          specialization, push_data_start_dword);
			if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv,
			                         options.maintenance8_enabled)) {
				DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
				EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
				     options.shader_hash);
			}
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		vk::ShaderModule module = nullptr;
		{
			const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Module);
			module = CompileSPV(result.spirv, graphics.device);
		}
		EXIT_IF(module == nullptr);
		ShaderDump::RecordModule(module, stage_name, options.shader_hash, result.spirv);
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .function_code  = {function_code.begin(), function_code.end()},
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = 0, .module = module},
		};
	}

	template <typename InputInfo>
	std::shared_ptr<TranslateJob>
	MakeJob(const ShaderParams& params, const InputInfo& input_info,
	        const ShaderRecompiler::CompileOptions& options, const char* stage_name,
	        const ShaderRecompiler::IR::ResourceSpecialization* specialization,
	        uint32_t push_data_cursor) {
		auto job      = std::make_shared<TranslateJob>();
		job->graphics = &graphics;
		job->code.assign(params.code.begin(), params.code.end());
		job->back_code.assign(params.back_code.begin(), params.back_code.end());
		job->user_data         = params.user_data;
		job->options           = options;
		job->options.user_data = std::span<const uint32_t>(job->user_data).first(params.user_data_count);
		job->options.back_code = job->back_code;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			job->vertex_input              = input_info;
			job->options.input_info.vertex = &job->vertex_input;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			job->pixel_input              = input_info;
			job->options.input_info.pixel = &job->pixel_input;
		} else {
			job->compute_input              = input_info;
			job->options.input_info.compute = &job->compute_input;
		}
		job->stage_name = stage_name;
		if (specialization != nullptr) {
			job->compile          = true;
			job->specialization   = *specialization;
			job->push_data_cursor = push_data_cursor;
		}
		return job;
	}

	void LogShaderCounts() const {
		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
	}

	static std::pair<const char*, const char*> StageLabels(ShaderType stage) {
		switch (stage) {
			case ShaderType::Vertex: return {"ShaderRecompiler VS", "vs"};
			case ShaderType::Mesh: return {"ShaderRecompiler MS", "ms"};
			case ShaderType::Local: return {"ShaderRecompiler LS", "ls"};
			case ShaderType::TessellationControl: return {"ShaderRecompiler HS", "hs"};
			case ShaderType::TessellationEvaluation: return {"ShaderRecompiler DS", "ds"};
			case ShaderType::Pixel: return {"ShaderRecompiler PS", "ps"};
			case ShaderType::Compute: return {"ShaderRecompiler CS", "cs"};
			default: EXIT("invalid pipeline shader stage\n");
		}
		return {};
	}

	// Everything the recompiler is told besides the program itself. The spans and input_info must
	// outlive the options (MakeJob re-points them at the job's own copies).
	template <typename InputInfo>
	ShaderRecompiler::CompileOptions MakeOptions(ShaderType stage, uint64_t hash,
	                                             std::span<const uint32_t> user_data,
	                                             std::span<const uint32_t> back_code,
	                                             const InputInfo& input_info, const char* label) const {
		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.maintenance8_enabled = graphics.maintenance8_enabled;
		options.device_features      = {
		         .fragment_shader_barycentric = graphics.optional_features.fragment_shader_barycentric,
		         .geometry_varying_locations  = graphics.optional_features.geometry_varying_locations,
		         .shader_float64              = graphics.optional_features.shader_float64,
		         .shader_shared_int64_atomics = graphics.optional_features.shader_shared_int64_atomics,
		         .signed_zero_inf_nan_preserve_f32 =
		             graphics.optional_features.shader_signed_zero_inf_nan_preserve_f32,
		         .subgroup_vertex       = graphics.SubgroupOpsSupported(vk::ShaderStageFlagBits::eVertex),
		         .subgroup_tessellation =
		             graphics.SubgroupOpsSupported(vk::ShaderStageFlagBits::eTessellationControl) &&
		             graphics.SubgroupOpsSupported(vk::ShaderStageFlagBits::eTessellationEvaluation),
		         .subgroup_fragment = graphics.SubgroupOpsSupported(vk::ShaderStageFlagBits::eFragment),
		         .subgroup_vote =
		             graphics.SubgroupOperationSupported(vk::SubgroupFeatureFlagBits::eVote),
		         .subgroup_arithmetic =
		             graphics.SubgroupOperationSupported(vk::SubgroupFeatureFlagBits::eArithmetic),
		         .storage_image_array_non_uniform_indexing =
		             graphics.optional_features.storage_image_array_non_uniform_indexing,
		         .mesh_shader       = graphics.mesh_shader_enabled,
		         .native_float_min_max = fast_paths.native_float_min_max,
		         .robust_buffer_loads  = fast_paths.robust_buffer_loads,
		         .lane_exec_branches   = fast_paths.lane_exec_branches,
		         .wqm_entry_lane_fold  = fast_paths.wqm_entry_lane_fold,
		         .render_scale      = RenderScale::Active(Config::GetRenderScale()),
		         .render_scale_keep_fragcoord =
		             (Config::GetRenderScaleFlags() & Config::RENDER_SCALE_FLAG_KEEP_FRAGCOORD) != 0,
		         .render_scale_keep_texels =
		             (Config::GetRenderScaleFlags() & Config::RENDER_SCALE_FLAG_KEEP_TEXELS) != 0,
		};
		options.device_quirks = device_quirks;
		options.shader_hash = hash;
		options.user_data   = user_data;
		options.back_code      = back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent ||
		                  IrDumpRequested(hash);
		options.early_dump  = options.dump_ir;
		options.emit_debug_info = Config::GraphicsDebugDumpEnabled();
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}

		return options;
	}

	// Returns the program for this draw, or a null program while it is still translating on a
	// worker (allow_async; the caller skips the draw). A caller that cannot skip (compute) gets
	// the translation on this thread.
	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor, bool allow_async) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		ShaderRecompiler::IR::SrtRuntime             runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .mapped                     = Libs::LibKernel::Memory::IsMappedRange,
		};
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			runtime.workgroup_counts = input_info.workgroup_counts;
			runtime.indirect_args_address = input_info.indirect_args_address;
		}
		bool source_changed = false;
		if (entry != programs.end()) {
			auto& cached = entry->second;
			Permutation* permutation = nullptr;
			if (cached.call_source) {
				source_changed = cached.RefreshCallSource(runtime);
				if (!source_changed) permutation = cached.FindPermutation<true>(runtime, push_data_cursor);
			} else {
				permutation = cached.FindPermutation<false>(runtime, push_data_cursor);
			}
			if (permutation != nullptr) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		const auto [label, stage_name] = StageLabels(stage);
		auto options = MakeOptions(stage, params.hash, user_data, params.back_code, input_info, label);

		// Installs a compiled permutation for this draw; the GPU thread owns the entry.
		const auto install = [&](Permutation&& compiled) -> ShaderProgram {
			compiled.handle.id = ++next_shader_id;
			entry->second.permutations.push_back(std::move(compiled));
			const auto& permutation = entry->second.permutations.back();
			input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
			permutation.program.bindings.AdvancePushData(push_data_cursor);
			LogShaderCounts();
			return permutation.handle;
		};

		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		// Blocking time on this thread: a synchronous translation, or the pickup of a worker's.
		std::optional<Common::PerfStats::ScopedStall> stall;
		// Sources that call other code are linked through guest memory on this thread (below).
		if (entry == programs.end() || !entry->second.call_source) {
			const bool async = allow_async && workers.Async();
			std::shared_ptr<TranslateJob> job;
			if (entry == programs.end()) {
				if (const auto pending = pending_programs.find(lookup_key); pending != pending_programs.end()) {
					job = pending->second;
					workers.Prioritize(job); // a disk cache warm-up job the game now needs
				}
			} else {
				job = entry->second.FindPendingCompile(push_data_cursor);
			}
			if (!job) {
				job = MakeJob(params, input_info, options, stage_name,
				              entry == programs.end() ? nullptr : &entry->second.specialization,
				              push_data_cursor);
				if (entry == programs.end()) {
					pending_programs.emplace(lookup_key, job);
				} else {
					entry->second.pending.push_back(job);
				}
				if (async) workers.Submit(job);
			}
			if (!async) {
				stall.emplace(Common::PerfStats::StallKind::ShaderTranslate, stage_name);
				workers.RunNow(job);
			}
			if (!job->Done()) {
				return {};
			}
			if (entry == programs.end()) {
				pending_programs.erase(lookup_key);
			} else {
				std::erase(entry->second.pending, job);
			}
			if (!job->needs_sync) {
				if (!stall) stall.emplace(Common::PerfStats::StallKind::ShaderTranslate, stage_name);
				if (job->compile) {
					auto compiled = std::move(*job->permutation);
					job->permutation.reset();
					return install(std::move(compiled));
				}
				RecordProgram(lookup_key, *job);
				entry = programs.try_emplace(lookup_key, std::move(job->plan), nullptr).first;
				{
					const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Materialize);
					EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
					    entry->second.resource_plan, runtime, entry->second.resources,
					    entry->second.specialization));
				}
				return install(CompilePermutation(graphics, stage_name, job->options,
				                                  std::move(job->translated),
				                                  entry->second.specialization, push_data_cursor, {}));
			}
		}
		if (!stall) stall.emplace(Common::PerfStats::StallKind::ShaderTranslate, stage_name);
		ShaderRecompiler::ShaderSource source;
		ShaderRecompiler::TranslateResult translated;
		{
			const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Translate);
			if (entry == programs.end()) source = ShaderRecompiler::PrepareShaderSource(params.code, options);
			auto* current = entry == programs.end() ? &source : entry->second.call_source.get();
			if (current != nullptr) {
				if (current->call) {
					if (entry == programs.end()) ShaderRecompiler::RefreshShaderSource(*current, runtime);
					translated = ShaderRecompiler::TranslateProgram(current->linked->decoded, options);
					translated.program.source_reads = current->reads;
				} else {
					translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
				}
			} else {
				translated = ShaderRecompiler::TranslateProgram(params.code, options);
			}
		}
		if (entry == programs.end()) {
			auto retained = source.call ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source)) : nullptr;
			ShaderRecompiler::IR::ResourcePlan plan;
			{
				const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Plan);
				plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			}
			entry = programs.try_emplace(lookup_key, std::move(plan), std::move(retained)).first;
			const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Materialize);
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization));
		} else if (source_changed) {
			{
				const Common::PerfStats::ScopedTranslatePhase phase(Common::PerfStats::TranslatePhase::Plan);
				entry->second.resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			}
			if (const auto* permutation = entry->second.FindPermutation<true>(runtime, push_data_cursor)) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}
		return install(CompilePermutation(graphics, stage_name, options, std::move(translated),
		                                  entry->second.specialization, push_data_cursor,
		                                  entry->second.FunctionCode()));
	}

	ProgramCache(GraphicContext& graphics, ShaderWorkerPool& workers)
	    : graphics(graphics), workers(workers),
	      device_quirks(ResolveShaderDeviceQuirks(graphics.physical_device)),
	      fast_paths(ResolveShaderFastPaths(graphics)) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		// The pool is stopped before this runs; jobs that never got installed release their own
		// modules.
		pending_programs.clear();
		for (auto& [key, entry]: programs) {
			(void)key;
			entry.pending.clear();
			for (const auto& permutation: entry.permutations) {
				graphics.device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	// --shader-disk-cache: appends each newly translated source; WarmUp replays the file.
	void OpenDiskCache(const std::string& title_id) {
		if (!workers.Async()) {
			PipelineCacheLog("Shader disk cache: disabled (needs --async-shaders workers)");
			return;
		}
		disk_cache = ShaderDiskCache::Open(std::filesystem::path("_ShaderCache") / (title_id + ".bin"),
		                                   OptionsSignature());
		if (!disk_cache) {
			return;
		}
		auto loaded = disk_cache->TakeLoaded();
		std::vector<std::shared_ptr<ShaderWorkerPool::Job>> warm_up;
		warm_up.reserve(loaded.size());
		for (auto& record: loaded) {
			std::visit([&](auto& input) { QueueWarmUp(record, input, warm_up); }, record.input);
		}
		PipelineCacheLog("Shader disk cache: warming up {} of {} programs on background workers",
		                 warm_up.size(), loaded.size());
		workers.SubmitBackground(std::move(warm_up));
	}

	// Everything besides the program inputs that changes translation output. A change starts the
	// cache over (replayed jobs use the current options anyway; this keeps the file honest).
	uint64_t OptionsSignature() const {
		ShaderVertexInputInfo dummy {};
		const auto  options = MakeOptions(ShaderType::Vertex, 0, {}, {}, dummy, "");
		const auto& f       = options.device_features;
		const auto  text    = fmt::format(
            "{} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} m8={} q={}", f.fragment_shader_barycentric,
            f.geometry_varying_locations, f.shader_float64, f.shader_shared_int64_atomics,
            f.signed_zero_inf_nan_preserve_f32, f.subgroup_vertex, f.subgroup_tessellation,
            f.subgroup_fragment, f.subgroup_vote, f.subgroup_arithmetic,
            f.storage_image_array_non_uniform_indexing, f.mesh_shader, f.native_float_min_max,
            f.robust_buffer_loads, f.lane_exec_branches, f.wqm_entry_lane_fold, f.render_scale,
            f.render_scale_keep_fragcoord, f.render_scale_keep_texels, options.maintenance8_enabled,
            options.device_quirks.branchless_bounds_checks);
		return XXH3_64bits(text.data(), text.size());
	}

	template <typename InputInfo>
	void QueueWarmUp(const ShaderDiskCache::Record& record, InputInfo& input_info,
	                 std::vector<std::shared_ptr<ShaderWorkerPool::Job>>& out) {
		ProgramKey key;
		key.stage           = record.stage;
		key.hash            = record.hash;
		key.user_data_count = record.user_data_count;
		key.code_size       = static_cast<uint32_t>(record.code.size());
		BuildStageStaticKey(input_info, key.static_state);
		if (!recorded.insert(key).second) {
			return; // duplicate record
		}
		ShaderParams params;
		params.code            = record.code;
		params.back_code       = record.back_code;
		params.user_data       = record.user_data;
		params.user_data_count = record.user_data_count;
		params.hash            = record.hash;
		const auto [label, stage_name] = StageLabels(record.stage);
		const auto user_data = std::span<const uint32_t>(params.user_data).first(params.user_data_count);
		const auto options =
		    MakeOptions(record.stage, record.hash, user_data, params.back_code, input_info, label);
		auto job = MakeJob(params, input_info, options, stage_name, nullptr, 0);
		pending_programs.emplace(std::move(key), job);
		out.push_back(std::move(job));
	}

	// The inputs of a source translated for the first time, for the next run's warm-up.
	void RecordProgram(const ProgramKey& key, const TranslateJob& job) {
		if (!disk_cache || !recorded.insert(key).second) {
			return;
		}
		ShaderDiskCache::Record record;
		record.stage           = key.stage;
		record.hash            = key.hash;
		record.user_data_count = key.user_data_count;
		std::copy(job.user_data.begin(), job.user_data.end(), record.user_data.begin());
		record.code      = job.code;
		record.back_code = job.back_code;
		if (key.stage == ShaderType::Pixel) {
			record.input = job.pixel_input;
		} else if (key.stage == ShaderType::Compute) {
			record.input = job.compute_input;
		} else {
			record.input = job.vertex_input;
		}
		disk_cache->Append(record);
	}

	std::unique_ptr<ShaderDiskCache>                            disk_cache;
	std::unordered_set<ProgramKey, ProgramKeyHash>              recorded;
	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	// Translations of sources without an entry yet, by the key they will get.
	std::unordered_map<ProgramKey, std::shared_ptr<TranslateJob>, ProgramKeyHash> pending_programs;
	ProgramKey                                                  lookup_key;
	GraphicContext&                                             graphics;
	ShaderWorkerPool&                                           workers;
	ShaderDeviceQuirks                                          device_quirks;
	ShaderFastPaths                                             fast_paths;
	uint64_t                                                    next_shader_id = 0;
};


namespace {

uint32_t ShaderWorkerThreads() {
	if (!Config::AsyncShadersEnabled()) {
		return 0;
	}
	const auto configured = Config::GetShaderThreads();
	return configured != 0 ? configured
	                       : ShaderWorkerPool::DefaultThreadCount(std::thread::hardware_concurrency());
}

} // namespace

// One pipeline creation (vkCreate*Pipelines and the layouts it needs) from copies of the state
// it reads. The stage runtimes inside vertex_info/pixel_info point at installed permutations and
// entries of the ProgramCache, which never move or disappear while the cache lives.
struct PipelineCache::PipelineJob: ShaderWorkerPool::Job {
	GraphicContext*   graphics     = nullptr;
	vk::PipelineCache driver_cache = nullptr;
	bool              compute      = false;
	// Graphics.
	PipelineRenderingState               rendering;
	PipelineVertexInputState             vertex_input;
	std::array<ShaderVertexInputInfo, 3> vertex_info {};
	uint32_t                             vertex_count = 0;
	std::optional<ShaderPixelInputInfo>  pixel_info;
	GraphicsPrograms                     programs;
	PipelineStaticParameters             static_params;
	// Compute (also the compute half of an emulated mesh draw).
	const ShaderRecompiler::IR::CompiledShaderInfo* program = nullptr;
	vk::ShaderModule                                module  = nullptr;
	// Result, valid once Done(); taken by the GPU thread at install.
	std::unique_ptr<Pipeline> result;

	~PipelineJob() override {
		if (result) {
			graphics->device.destroyPipeline(result->pipeline, nullptr);
			graphics->device.destroyPipelineLayout(result->pipeline_layout, nullptr);
			graphics->device.destroyDescriptorSetLayout(result->descriptor_set_layout, nullptr);
		}
	}

	void Run() override {
		auto pipeline = std::make_unique<Pipeline>();
		if (compute) {
			CreatePipelineInternal(*graphics, *pipeline, *program, module, driver_cache);
		} else {
			CreatePipelineInternal(*graphics, *pipeline, rendering, vertex_input,
			                       std::span<const ShaderVertexInputInfo>(vertex_info.data(), vertex_count),
			                       pixel_info ? &*pixel_info : nullptr, programs, static_params,
			                       driver_cache);
		}
		result = std::move(pipeline);
	}
};

template <typename Key, typename Hash, typename MakeJob>
PipelineCache::Pipeline* PipelineCache::PollPipeline(
    std::unordered_map<Key, std::unique_ptr<Pipeline>, Hash>&   installed,
    std::unordered_map<Key, std::shared_ptr<PipelineJob>, Hash>& pending, const Key& key,
    MakeJob&& make_job, bool blocking) {
	if (const auto found = installed.find(key); found != installed.end()) {
		return found->second.get();
	}
	auto                         pending_it = pending.find(key);
	std::shared_ptr<PipelineJob> job;
	if (pending_it != pending.end()) {
		job = pending_it->second;
	} else {
		job        = make_job();
		pending_it = pending.emplace(key, job).first;
		if (m_workers->Async() && !blocking) {
			m_workers->Submit(job);
		}
	}
	if (blocking || !m_workers->Async()) {
		m_workers->RunNow(job);
	}
	if (!job->Done()) {
		return nullptr;
	}
	pending.erase(pending_it);
	EXIT_NOT_IMPLEMENTED(!job->result || job->result->pipeline == nullptr ||
	                     job->result->pipeline_layout == nullptr);
	auto [iter, inserted] = installed.emplace(key, std::move(job->result));
	EXIT_IF(!inserted);
	CheckpointDriverCache();
	return iter->second.get();
}

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_workers(std::make_unique<ShaderWorkerPool>(ShaderWorkerThreads())),
      m_program_cache(std::make_unique<ProgramCache>(graphics, *m_workers)),
      m_next_driver_cache_save(std::chrono::steady_clock::now() + DriverCacheSaveInterval) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	PipelineCacheLog("Shader workers: {} ({})", m_workers->ThreadCount(),
	                 m_workers->Async() ? "async shaders: draws skip pipelines that are not ready"
	                                    : "synchronous");
	InitializeDriverCache();
	if (Config::ShaderDiskCacheEnabled()) {
		if (const auto title_id = PipelineCacheTitleId(); !title_id.empty()) {
			m_program_cache->OpenDiskCache(title_id);
		}
	}
}

PipelineCache::~PipelineCache() {
	// Stop the workers first: a job still running owns Vulkan objects it must finish creating,
	// and the pending maps below release the ones nobody installed.
	m_workers.reset();
	m_pending_graphics.clear();
	m_pending_compute.clear();
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	m_program_cache.reset();
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	// One file per driver build: a cache written by another driver (the system driver vs an
	// imported Turnip) fails the signature check, and alternating drivers would otherwise discard
	// each other's cache on every launch and recompile every pipeline (seconds each on mobile).
	const auto& device_properties = m_graphics.GetPhysicalDeviceProperties();
	m_driver_cache_path           = std::filesystem::path("_PipelineCache") /
	                      fmt::format("{}-{:08x}-{:08x}.bin", title_id, device_properties.deviceID,
	                                  device_properties.driverVersion);
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::CheckpointDriverCache() {
	if (m_driver_cache == nullptr) {
		return;
	}
	m_driver_cache_dirty = true;
	if (std::chrono::steady_clock::now() < m_next_driver_cache_save) {
		return;
	}
#if defined(__ANDROID__)
	// Android ends apps without running destructors, so new pipelines are written out in the
	// background, at most once per DriverCacheSaveInterval, instead of only at shutdown.
	if (m_flush_thread.joinable()) {
		m_flush_thread.join();
	}
	m_next_driver_cache_save = std::chrono::steady_clock::now() + DriverCacheSaveInterval;
	m_driver_cache_dirty     = false;
	m_flush_thread           = std::thread([this] { (void)WriteDriverCacheFile(); });
#else
	Save();
#endif
}

void PipelineCache::Save() {
	if (m_flush_thread.joinable()) {
		m_flush_thread.join();
	}
	if (m_driver_cache == nullptr || !m_driver_cache_dirty) {
		return;
	}
	m_next_driver_cache_save = std::chrono::steady_clock::now() + DriverCacheSaveInterval;
	if (WriteDriverCacheFile()) {
		m_driver_cache_dirty = false;
	}
}

// Serializes the driver cache to disk without releasing it. vkGetPipelineCacheData does not need
// external synchronization against pipeline creation, so this may run on a worker thread.
bool PipelineCache::WriteDriverCacheFile() const {
	if (m_driver_cache == nullptr) {
		return false;
	}
	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return false;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	const bool written = prefix_written == prefix.size() && payload_written == payload.size() && flushed;
	std::error_code error;
	if (written) {
		std::filesystem::rename(temp_path, m_driver_cache_path, error);
	}
	if (!written || error) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	return true;
}

// Mesh emulation runs the mesh workgroup as a compute workgroup, so it is bound by the compute
// limits instead of the mesh ones. The host subgroup size follows the compute stage.
static void CheckMeshEmulationLimits(const GraphicContext& graphics, ShaderMeshInputInfo& mesh) {
	mesh.host_subgroup_size = graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	const auto logical_threads = mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
	const auto host_threads    = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
	                          std::min(mesh.host_subgroup_size, mesh.wave_size);
	// Guest LDS, the allocation pair and the shared per-vertex Layer staging.
	const auto shared_bytes = (static_cast<uint64_t>(mesh.lds_size_dwords) + 2u +
	                           mesh.max_vertices) * sizeof(uint32_t);
	if (host_threads > limits.maxComputeWorkGroupInvocations ||
	    host_threads > limits.maxComputeWorkGroupSize[0] ||
	    shared_bytes > limits.maxComputeSharedMemorySize) {
		EXIT("mesh emulation: mesh stage exceeds host compute limits: threads=%u (max %u) "
		     "shared=%" PRIu64 " bytes (max %u) vertices=%u primitives=%u\n",
		     host_threads, limits.maxComputeWorkGroupInvocations, shared_bytes,
		     limits.maxComputeSharedMemorySize, mesh.max_vertices, mesh.max_primitives);
	}
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info,
    const ShaderIndirectDrawInfo& indirect) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
		// --gpu-indirect is translated for the plain vertex stage and for emulated mesh stages
		// (not fast launch); the caller falls back to CPU-read arguments for the others.
		const auto& stage_info = vertex_info[0];
		if (stage_info.logical_stage == ShaderType::Vertex ||
		    (stage_info.logical_stage == ShaderType::Mesh && !m_graphics.mesh_shader_enabled &&
		     !stage_info.mesh.fast_launch)) {
			vertex_info[0].indirect = indirect;
		}
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	const bool object_primitive_id = context.HasObjectPrimitiveIdOutput();
	if (object_primitive_id) {
		// CxObjectIdMode: register source plus a 32-bit pipelined primitive-ID addition.
		EXIT_NOT_IMPLEMENTED(sh.m_paClObjprimIdCntl != 2u || mesh_active || tess_active ||
		                     !pixel_active || (sh.m_vgtDrawPayloadCntl & 8u) == 0 ||
		                     user_config.GetPrimType() != Prospero::PrimitiveType::kTriList);
		const auto& target = context.GetRenderTarget(7);
		EXIT_NOT_IMPLEMENTED(Prospero::ResolveRenderTargetFormat(target.info.format,
		                                                       target.info.channel_type)
		                         .buffer_format != Prospero::BufferFormat::k32UInt);
	}
	const bool mesh_emulated = mesh_active && !m_graphics.mesh_shader_enabled;
	if (mesh_emulated) {
		CheckMeshEmulationLimits(m_graphics, vertex_info[0].mesh);
	} else if (mesh_active) {
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		pixel_info.object_primitive_id = object_primitive_id;
		for (uint32_t export_index = 0; export_index < 8u; export_index++) {
			const auto slot = ShaderPixelExportTarget(pixel_info.target_shader_mask, export_index);
			if (slot >= 8u || pixel_info.target_output_mode[export_index] == 0u) continue;
			const auto& rt = context.GetRenderTarget(slot);
			const auto target_mask = render_target_mask_slot(context.GetRenderTargetMask(), slot);
			if (rt.base.addr == 0 || target_mask == 0u) continue;
			const auto format = Prospero::ResolveRenderTargetFormat(rt.info.format, rt.info.channel_type);
			if (format.buffer_format != Prospero::BufferFormat::k11_11_10UNorm) continue;
			const auto mapping = pixel_info.target_export_mapping[export_index];
			const auto shader_mask = render_target_mask_slot(pixel_info.target_shader_mask, slot);
			const bool blend = context.GetBlendControl(slot).enable && !rt.info.blend_bypass;
			if (blend || (mapping.ApplyMask(target_mask) & 7u) != 7u ||
			    (mapping.ApplyMask(shader_mask) & 7u) != 7u ||
			    pixel_info.target_output_mode[export_index] != 9u) {
				EXIT("unsupported packed UNorm render-target export: slot=%u export=%u mode=%u "
				     "target_mask=0x%x shader_mask=0x%x mapping=0x%02x blend=%u\n",
				     slot, export_index, pixel_info.target_output_mode[export_index], target_mask,
				     shader_mask, mapping.packed, static_cast<uint32_t>(blend));
			}
			pixel_info.target_packed_unorm_mask |= 1u << export_index;
			pixel_info.target_round_mode |= static_cast<uint8_t>(rt.info.round_mode) << export_index;
		}
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	uint32_t push_data_cursor =
	    mesh_emulated ? ShaderRecompiler::IR::PushData::MeshEmulationDrawDwordCount
	                  : ShaderRecompiler::IR::PushData::GraphicsDrawDwords(mesh_active,
	                                                                       object_primitive_id);
	GraphicsPrograms  result;
	// Every stage is requested even when an earlier one is still translating, so the workers
	// translate them in parallel; the draw is skipped until all are installed.
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor, true);
		result.ready = result.ready && static_cast<bool>(result.pixel);
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] =
		    m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor, true);
		result.ready = result.ready && static_cast<bool>(result.vertex[i]);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t          push_data_cursor = 0;
	// The dispatch's results feed the game: it waits for its program.
	return m_program_cache->Get(params, input_info, push_data_cursor, false);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline* PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		if (colors[i].desc.info.guest_format == Prospero::BufferFormat::k11_11_10UNorm) {
			static_params.color_mask[slot] = 1u;
		}
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	return PollPipeline(m_graphics_pipelines, m_pending_graphics, key, [&] {
		if (graphics_debug_dump_enabled()) {
			ShaderDbgDumpInputInfo(vs_input_info);
			if (ps_active) {
				ShaderDbgDumpInputInfo(*ps_input_info);
			}
			LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
			     vs_id, static_cast<void*>(vertex_program.module), ps_id,
			     static_cast<void*>(pixel_program.module));
		}
		LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
		auto job          = std::make_shared<PipelineJob>();
		job->graphics     = &m_graphics;
		job->driver_cache = m_driver_cache;
		job->rendering    = key.rendering;
		job->vertex_input = key.vertex_input;
		EXIT_IF(vertex_info.size() > job->vertex_info.size());
		job->vertex_count = static_cast<uint32_t>(vertex_info.size());
		std::copy(vertex_info.begin(), vertex_info.end(), job->vertex_info.begin());
		if (ps_active) {
			job->pixel_info = *ps_input_info;
		}
		job->programs      = programs;
		job->static_params = key.static_params;
		return job;
	}, /*blocking=*/false);
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);
	auto* pipeline = PollPipeline(m_compute_pipelines, m_pending_compute, compute_program.id, [&] {
		if (graphics_debug_dump_enabled()) {
			ShaderDbgDumpInputInfo(input_info);
		}
		EXIT_IF(!input_info.stage);
		auto job          = std::make_shared<PipelineJob>();
		job->graphics     = &m_graphics;
		job->driver_cache = m_driver_cache;
		job->compute      = true;
		job->program      = input_info.stage.program;
		job->module       = compute_program.module;
		return job;
	}, /*blocking=*/true);
	EXIT_IF(pipeline == nullptr);
	return *pipeline;
}

PipelineCache::Pipeline*
PipelineCache::TryGetMeshEmulationPipeline(const ShaderVertexInputInfo& mesh_input,
                                           const ShaderProgram&         mesh_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(MeshEmulation)", profiler::colors::RedA100);
	EXIT_IF(!mesh_program || !mesh_input.stage ||
	        !mesh_input.stage.program->info.mesh_emulation);
	// Program ids are unique across stages, so emulated mesh programs share the compute map.
	return PollPipeline(m_compute_pipelines, m_pending_compute, mesh_program.id, [&] {
		auto job          = std::make_shared<PipelineJob>();
		job->graphics     = &m_graphics;
		job->driver_cache = m_driver_cache;
		job->compute      = true;
		job->program      = mesh_input.stage.program;
		job->module       = mesh_program.module;
		LOGF("Mesh emulation: compute pipeline MS=0x%016" PRIx64 " threads=%u vertices=%u "
		     "primitives=%u\n",
		     mesh_input.stage.program->shader_hash, mesh_input.mesh.threads_num[0],
		     mesh_input.mesh.max_vertices, mesh_input.mesh.max_primitives);
		return job;
	}, /*blocking=*/false);
}

PipelineCache::Pipeline&
PipelineCache::GetMeshEmulationPipeline(const ShaderVertexInputInfo& mesh_input,
                                        const ShaderProgram&         mesh_program) {
	EXIT_IF(!mesh_program || !mesh_input.stage ||
	        !mesh_input.stage.program->info.mesh_emulation);
	auto* pipeline = PollPipeline(m_compute_pipelines, m_pending_compute, mesh_program.id, [&] {
		auto job          = std::make_shared<PipelineJob>();
		job->graphics     = &m_graphics;
		job->driver_cache = m_driver_cache;
		job->compute      = true;
		job->program      = mesh_input.stage.program;
		job->module       = mesh_program.module;
		return job;
	}, /*blocking=*/true);
	EXIT_IF(pipeline == nullptr);
	return *pipeline;
}

} // namespace Libs::Graphics
