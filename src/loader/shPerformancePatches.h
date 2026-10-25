#ifndef KYTY_LOADER_SH_PERFORMANCE_PATCHES_H_
#define KYTY_LOADER_SH_PERFORMANCE_PATCHES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Exact PPSA10112 executable supplied for binary analysis. These patches change
// engine CVar registrations, not shader IDs, render output or generic shared constants.
namespace Loader::ShPerformancePatches {
inline constexpr uint64_t kEntry           = 0x80;
inline constexpr size_t   kTextSize        = 0x57554cc;
inline constexpr uint64_t kTextHash        = 0x10ca182d422310d0ull;
inline constexpr uint64_t kReadOnlyRva     = 0x5758000;
inline constexpr size_t   kReadOnlySize    = 0x1b0b243;
inline constexpr uint64_t kGlobalsRva      = 0x7b10000;
inline constexpr size_t   kGlobalsSize     = 0x29880;
inline constexpr size_t   kFogOffset       = 0x17f4c;
inline constexpr uint32_t kConsolePriority = 0x09000000;

enum class Effect : size_t { Shadows, Fog, SSR, AO, Bloom, DOF, MotionBlur, RT, Count };
inline constexpr size_t kEffectCount = static_cast<size_t>(Effect::Count);
inline constexpr std::array<const char*, kEffectCount> kEnvironment {
    {"KYTY_SH_NO_SHADOWS", "KYTY_SH_NO_FOG", "KYTY_SH_NO_SSR", "KYTY_SH_NO_AO", "KYTY_SH_NO_BLOOM",
	 "KYTY_SH_NO_DOF", "KYTY_SH_NO_MOTION_BLUR", "KYTY_SH_NO_RT"}};
struct Options {
	unsigned screen_percent = 0; // 0 preserves the game's own screen percentage.
	std::array<bool, kEffectCount> disable {};
	[[nodiscard]] bool             Any() const {
		if (screen_percent) return true;
		for (bool value: disable)
			if (value) return true;
		return false;
	}
};
[[nodiscard]] inline bool ValidOptions(const Options& options) {
	return options.screen_percent == 0 || options.screen_percent == 25 ||
	       options.screen_percent == 50 || options.screen_percent == 75;
}
[[nodiscard]] inline uint64_t Fingerprint(std::span<const uint8_t> text) {
	uint64_t hash = 14695981039346656037ull;
	for (uint8_t byte: text) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hash;
}
[[nodiscard]] inline bool MatchesIdentity(std::string_view title, bool main, uint64_t entry,
                                          std::span<const uint8_t> pristine_text) {
	return title == "PPSA10112" && main && entry == kEntry && pristine_text.size() == kTextSize &&
	       Fingerprint(pristine_text) == kTextHash;
}

namespace Detail {
struct Registration {
	Effect           effect;
	size_t           begin;
	std::string_view original;
	size_t           value; // immediate in MOV EDX, imm32, or 0 when already zero/external.
	size_t           flags; // immediate in MOV R8D, imm32.
};
inline constexpr std::array<Registration, kEffectCount> kRegistrations {{
    {Effect::Shadows, 0x122bcad, "488d35a47f7505488d0d29837f05ba0500000041b860000000ff5008",
	 0x122bcbc, 0x122bcc2},
    {Effect::Fog, 0x2ca4321, "488d359e17cb03488d151d3ce804488d0d5e56d70341b860000000ff5030", 0,
	 0x2ca4338},
    {Effect::SSR, 0x2b615f0, "488d35b9c9ef03488d0d4c15fd03ba0300000041b860000000ff5008", 0x2b615ff,
	 0x2b61605},
    {Effect::AO, 0x25df2c6, "488d35d7033604488d0d2ad84804baffffffff41b860000000ff5008", 0x25df2d5,
	 0x25df2db},
    {Effect::Bloom, 0x122bae4, "488d35fdaa7705488d0ddab68105ba0500000041b860000000ff5008",
	 0x122baf3, 0x122baf9},
    {Effect::DOF, 0x122bfc5, "488d35a8577e05488d0d877a8b05ba0200000041b860000000ff5008", 0x122bfd4,
	 0x122bfda},
    {Effect::MotionBlur, 0x122bd1f, "488d35ac577e05488d0dd9a87705ba0400000041b860000000ff5008",
	 0x122bd2e, 0x122bd34},
    {Effect::RT, 0x262ff97, "488d3510514a04488d0dc77c4b0441b82400000031d2ff5008", 0, 0x262ffa7},
}};
inline constexpr size_t                                 kResolutionBegin = 0x122c034;
inline constexpr std::string_view                       kResolutionOriginal =
    "c5fa10051c466e05488b07488d3560259105488d1553577e05b940000000ff5010";
inline constexpr size_t           kResolutionDisp   = 0x122c038;
inline constexpr size_t           kResolutionFlags  = 0x122c04e;
inline constexpr uint64_t         kOriginalFloatRva = 0x6910658;
inline constexpr size_t           kDynamicResBegin  = 0x4025fac;
inline constexpr std::string_view kDynamicResOriginal =
    "488d3529f7a402488d0d80cead0241b82000000031d2ff5008";
inline constexpr size_t       kDynamicResFlags = 0x4025fbc;
[[nodiscard]] inline uint64_t FloatRva(unsigned percent) {
	return percent == 75 ? 0x6906a20 : percent == 50 ? 0x6910280 : 0x6905ec4;
}
inline uint32_t Read32(std::span<const uint8_t> bytes, size_t offset) {
	uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= uint32_t(bytes[offset + i]) << (i * 8);
	return value;
}
inline void Write32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
	for (unsigned i = 0; i < 4; ++i)
		bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
inline unsigned Nibble(char c) {
	return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
}
[[nodiscard]] inline bool Matches(std::span<const uint8_t> bytes, size_t offset,
                                  std::string_view hex) {
	if ((hex.size() & 1) || offset > bytes.size() || hex.size() / 2 > bytes.size() - offset)
		return false;
	for (size_t i = 0; i < hex.size() / 2; ++i)
		if (bytes[offset + i] != ((Nibble(hex[2 * i]) << 4) | Nibble(hex[2 * i + 1]))) return false;
	return true;
}
inline uint32_t FloatBits(unsigned percent) {
	return percent == 75 ? 0x42960000 : percent == 50 ? 0x42480000 : 0x41c80000;
}
} // namespace Detail

// Validate every selected registration and required literal/storage before any
// mutation. The loader additionally requires the complete pristine text hash.
[[nodiscard]] inline bool ValidateSites(std::span<const uint8_t> text,
                                        std::span<const uint8_t> rodata,
                                        std::span<const uint8_t> globals, const Options& options) {
	if (!ValidOptions(options) || text.size() != kTextSize || rodata.size() != kReadOnlySize ||
	    globals.size() != kGlobalsSize)
		return false;
	if (options.screen_percent) {
		if (!Detail::Matches(text, Detail::kResolutionBegin, Detail::kResolutionOriginal) ||
		    !Detail::Matches(text, Detail::kDynamicResBegin, Detail::kDynamicResOriginal) ||
		    Detail::Read32(rodata, Detail::kOriginalFloatRva - kReadOnlyRva) != 0x42c80000 ||
		    Detail::Read32(rodata, Detail::FloatRva(options.screen_percent) - kReadOnlyRva) !=
		        Detail::FloatBits(options.screen_percent))
			return false;
	}
	for (const auto& registration: Detail::kRegistrations) {
		if (options.disable[static_cast<size_t>(registration.effect)] &&
		    !Detail::Matches(text, registration.begin, registration.original))
			return false;
	}
	if (options.disable[static_cast<size_t>(Effect::Fog)] &&
	    Detail::Read32(globals, kFogOffset) != 1)
		return false;
	return true;
}
[[nodiscard]] inline bool ApplySites(std::span<uint8_t> text, std::span<const uint8_t> rodata,
                                     std::span<uint8_t> globals, const Options& options) {
	if (!ValidateSites(text, rodata, globals, options)) return false;
	if (options.screen_percent) {
		const int64_t displacement = int64_t(Detail::FloatRva(options.screen_percent)) - 0x122c03c;
		Detail::Write32(text, Detail::kResolutionDisp, static_cast<uint32_t>(displacement));
		Detail::Write32(text, Detail::kResolutionFlags, kConsolePriority | 0x40);
		Detail::Write32(text, Detail::kDynamicResFlags, kConsolePriority | 0x20);
	}
	for (const auto& registration: Detail::kRegistrations) {
		if (!options.disable[static_cast<size_t>(registration.effect)]) continue;
		if (registration.value) Detail::Write32(text, registration.value, 0);
		const uint32_t flags = Detail::Read32(text, registration.flags);
		Detail::Write32(text, registration.flags, (flags & 0x00ffffffu) | kConsolePriority);
	}
	if (options.disable[static_cast<size_t>(Effect::Fog)]) Detail::Write32(globals, kFogOffset, 0);
	return true;
}
} // namespace Loader::ShPerformancePatches
#endif
