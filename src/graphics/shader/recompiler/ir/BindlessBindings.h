#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Descriptor set 1 of a pipeline that samples bindless images (the host side is BindlessTable).
inline constexpr uint32_t BindlessDescriptorSet  = 1;
inline constexpr uint32_t BindlessImages2D       = 0;
inline constexpr uint32_t BindlessImages2DArray  = 1;
inline constexpr uint32_t BindlessImagesCube     = 2;
inline constexpr uint32_t BindlessImages3D       = 3;
inline constexpr uint32_t BindlessTranslation    = 4;
inline constexpr uint32_t BindlessFeedback       = 5;
// Samplers mirrored from guest sampler heaps (16-byte S# records); slot 0 is the default sampler.
inline constexpr uint32_t BindlessSamplers       = 6;
// A translation entry for a key whose texture is not resident yet; the shader samples slot 0.
inline constexpr uint32_t BindlessPending        = 0xffffffffu;
// Feedback flags a shader stores per key it samples: the texture is pending, or resident and used.
inline constexpr uint32_t BindlessFlagPending    = 1;
inline constexpr uint32_t BindlessFlagUsed       = 2;
// The S# a sampler without a usable heap entry gets: wrap on every axis, trilinear, full LOD range.
inline constexpr std::array<uint32_t, 4> BindlessDefaultSampler {
    0x00000000u,                             // wrap on every axis
    0xfffu << 12u,                           // max_lod 255.9
    (1u << 20u) | (1u << 22u) | (2u << 26u), // bilinear mag/min, linear mip
    0x00000000u};

// The sampler used where a bindless sampler cannot be resolved (BindlessDefaultSampler, and the
// shader-side fallbacks in ResourceTracking). KYTY_DEFAULT_SAMPLER_CLAMP=1 (environment, read
// once) clamps to the edge texel on every axis instead of wrapping: a texture meant to be drawn
// once (Wolverine's moon) wrapped repeats across the whole surface.
inline const std::array<uint32_t, 4>& FallbackSampler() {
	static const std::array<uint32_t, 4> words = [] {
		auto        result = BindlessDefaultSampler;
		const char* clamp  = std::getenv("KYTY_DEFAULT_SAMPLER_CLAMP");
		if (clamp != nullptr && std::strcmp(clamp, "0") != 0) {
			result[0] = 2u | (2u << 3u) | (2u << 6u); // CLAMP_X/Y/Z = clamp to last texel
		}
		return result;
	}();
	return words;
}

// Title workaround, separate from the generic fallback: KYTY_CLAMP_SAMPLER_SHADERS=hash,hash,...
// (hex, environment) gives the shaders listed a clamping fallback sampler while every other
// shader keeps the wrapping one. Wolverine's night sky draws its moon through an unresolved
// bindless sampler; wrap tiles it across the sky, but terrain needs wrap.
inline const std::array<uint32_t, 4>& FallbackSamplerFor(uint64_t shader_hash) {
	static const std::unordered_set<uint64_t> clamped = [] {
		std::unordered_set<uint64_t> result;
		const char*                  list = std::getenv("KYTY_CLAMP_SAMPLER_SHADERS");
		if (list != nullptr) {
			std::string text(list);
			size_t      start = 0;
			while (start < text.size()) {
				const auto end = text.find(',', start);
				const auto item =
				    text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (!item.empty()) {
					result.insert(std::strtoull(item.c_str(), nullptr, 16));
				}
				if (end == std::string::npos) {
					break;
				}
				start = end + 1;
			}
		}
		return result;
	}();
	static const std::array<uint32_t, 4> clamp = [] {
		auto result = BindlessDefaultSampler;
		result[0]   = 2u | (2u << 3u) | (2u << 6u);
		return result;
	}();
	return clamped.contains(shader_hash) ? clamp : FallbackSampler();
}

// Research: the tail of the feedback buffer holds loop-watchdog trip reports. Word
// WatchdogReportBase counts reports; report n is WatchdogReportWords words starting at
// WatchdogReportBase + WatchdogReportWords * (n + 1). No heap region reaches this far.
inline constexpr uint32_t WatchdogReportBase     = 0xf0000u;
inline constexpr uint32_t WatchdogReportWords    = 64;
inline constexpr uint32_t WatchdogReportSlots    = 1023;
inline constexpr uint32_t WatchdogReportPhis     = 24; // header phi values per lane half

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
