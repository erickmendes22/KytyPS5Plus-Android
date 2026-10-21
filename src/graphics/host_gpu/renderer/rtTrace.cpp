#include "graphics/host_gpu/renderer/rtTrace.h"

#include "common/perfWaits.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Libs::Graphics::RtTrace {

namespace {

constexpr uint32_t MaxSources     = 6;    // source dispatches traced
constexpr uint32_t FollowBindings = 4000; // later stage bindings checked after a source (until the next one)
constexpr uint32_t MaxUseLines    = 900;
constexpr uint32_t MaxGeneration  = 2;    // follow images written by readers of the source's outputs, one level

struct Tracked {
	uint64_t address    = 0;
	uint64_t size       = 0;
	uint32_t generation = 0; // 1 = written by the source, 2 = written by a pass that read a generation-1 image
};

struct State {
	std::vector<std::string> spec;
	std::vector<Tracked>     tracked;
	uint32_t                 sources   = 0;
	uint32_t                 following = 0;
	uint32_t                 ordinal   = 0;
	uint32_t                 use_lines = 0;
};

State& Get() {
	static State state = [] {
		State s;
		if (const char* value = std::getenv("KYTY_RT_TRACE"); value != nullptr) {
			s.spec = PipelineStats::ParseSpec(value);
		}
		return s;
	}();
	return state;
}

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Compute: return "CS";
		case ShaderType::Pixel: return "PS";
		case ShaderType::Vertex: return "VS";
		default: return "other";
	}
}

bool IsStorage(const TextureBinding& binding) {
	return binding.desc.type == TextureCache::BindingType::Storage;
}

std::string Describe(const TextureBinding& binding) {
	const auto& info = binding.desc.info;
	char line[320];
	std::snprintf(line, sizeof(line),
	              "%s addr=0x%010" PRIx64 " size=0x%" PRIx64 " %ux%ux%u levels=%u layers=%u fmt=%s tile=%u",
	              IsStorage(binding) ? "storage" : "sampled", info.data.address, info.data.size,
	              info.extent.width, info.extent.height, info.extent.depth, info.resources.levels,
	              info.resources.layers, vk::to_string(info.pixel_format).c_str(),
	              static_cast<uint32_t>(info.tile_mode));
	return line;
}

// Generation of the tracked image this binding overlaps, 0 when none.
uint32_t TrackedGeneration(const State& state, const TextureBinding& binding) {
	const auto& data = binding.desc.info.data;
	uint32_t    best = 0;
	for (const auto& t: state.tracked) {
		if (data.size != 0 && data.address < t.address + t.size && t.address < data.address + data.size &&
		    (best == 0 || t.generation < best)) {
			best = t.generation;
		}
	}
	return best;
}

void Emit(const char* format, auto... args) {
	char line[512];
	std::snprintf(line, sizeof(line), format, args...);
	Common::PerfStats::EmitLine(line);
}

void StartSource(State& state, uint64_t shader_hash, ShaderType stage, std::span<const TextureBinding> images) {
	state.sources++;
	state.tracked.clear();
	state.following = FollowBindings;
	state.ordinal   = 0;
	for (uint32_t i = 0; i < images.size(); i++) {
		Emit("[rt-trace] src #%u hash=0x%016" PRIx64 " %s slot=%u %s", state.sources, shader_hash,
		     StageName(stage), i, Describe(images[i]).c_str());
		if (IsStorage(images[i])) {
			state.tracked.push_back({images[i].desc.info.data.address, images[i].desc.info.data.size, 1});
		}
	}
}

// A binding set that touches a tracked image is logged whole (every slot, tracked ones marked with
// their generation), so a reader of the RT output that also reads and writes a same-shaped history
// image shows up as a temporal pass.
void LogUse(State& state, uint64_t shader_hash, ShaderType stage, std::span<const TextureBinding> images) {
	uint32_t reads_generation = 0;
	for (const auto& image: images) {
		const uint32_t g = TrackedGeneration(state, image);
		if (g != 0 && (reads_generation == 0 || g < reads_generation)) {
			reads_generation = g;
		}
	}
	if (reads_generation == 0) {
		return;
	}
	for (uint32_t i = 0; i < images.size() && state.use_lines < MaxUseLines; i++) {
		state.use_lines++;
		Emit("[rt-trace] use +%u src=%u hash=0x%016" PRIx64 " %s slot=%u gen=%u %s", state.ordinal,
		     state.sources, shader_hash, StageName(stage), i, TrackedGeneration(state, images[i]),
		     Describe(images[i]).c_str());
	}
	if (reads_generation < MaxGeneration) {
		for (const auto& image: images) {
			if (IsStorage(image) && TrackedGeneration(state, image) == 0) {
				state.tracked.push_back({image.desc.info.data.address, image.desc.info.data.size, reads_generation + 1});
			}
		}
	}
}

} // namespace

void OnStageBindings(uint64_t shader_hash, ShaderType stage, std::span<const TextureBinding> images) {
	auto& state = Get();
	if (state.spec.empty()) {
		return;
	}
	if (PipelineStats::Matches(state.spec, shader_hash)) {
		if (state.sources < MaxSources) {
			StartSource(state, shader_hash, stage, images);
		} else {
			state.following = 0;
		}
		return;
	}
	if (state.following == 0 || state.use_lines >= MaxUseLines) {
		return;
	}
	state.following--;
	state.ordinal++;
	LogUse(state, shader_hash, stage, images);
}

} // namespace Libs::Graphics::RtTrace
