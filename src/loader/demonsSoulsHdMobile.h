#pragma once
// Signature-gated PPSA01341 01.007.000 native 1080p video mode.
// PC parity: chenxiao07/KytyPS5 DemonsSoulsHdOutput, issue #138.
// The game has INDEPENDENT video/renderer/UI modes. Setting only the video
// surface dimensions is incorrect; a 1080p output with native mode 2 also
// leaves equipment-menu 3D black. Mode 1 renders 1600x900 -> 1920x1080.
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>

namespace Loader::DemonsSoulsHdMobile {
inline constexpr uint32_t OutputWidth = 1920, OutputHeight = 1080;
inline constexpr uint32_t VideoMode = 1;
inline constexpr uint64_t PackedCeiling =
    (uint64_t{OutputHeight} << 32u) | OutputWidth;

inline constexpr std::string_view VideoChoice =
    "be 03 00 00 00 85 c9 0f 45 f1 85 c0 0f 45 f0 e8";
inline constexpr std::string_view RendererChoice =
    "be 03 00 00 00 ?? ?? ?? ?? ?? ?? ?? ?? 8b 8f 38 0d 00 00 8b 87 30 0d 00 00 85 c9 0f 45 f1 85 c0 0f 45 f0 39 b7";
inline constexpr std::string_view InterfaceChoice =
    "b8 03 00 00 00 85 c9 0f 45 c1 45 85 ff 48 8b 0a 41 0f 45 c7 83 c0 fd 83 f8 03";
inline constexpr std::string_view DisplayBuffer =
    "bf 00 0f 00 00 be 70 08 00 00 ba 01 00 00 00 b9 01 00 00 00";
inline constexpr std::string_view SetterFixedPair =
    "c5 f8 28 c8 48 b9 00 0f 00 00 70 08 00 00 48 ba";
inline constexpr std::string_view ConstructorFixedPair =
    "4c 8b 35 ?? ?? ?? ?? 48 b9 00 0f 00 00 70 08 00 00";
inline constexpr std::string_view QueryFixedPair =
    "89 7e 04 48 ba 00 0f 00 00 70 08 00 00 48 bf";

inline constexpr int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline constexpr size_t SignatureBytes(std::string_view pattern) {
    size_t count = 0;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') { ++i; continue; }
        if (i + 1 >= pattern.size()) return 0;
        const bool wildcard = pattern[i] == '?' && pattern[i + 1] == '?';
        if (!wildcard && (HexDigit(pattern[i]) < 0 || HexDigit(pattern[i + 1]) < 0))
            return 0;
        ++count;
        i += 2;
    }
    return count;
}
inline bool Matches(std::span<const uint8_t> bytes, std::string_view pattern) {
    if (bytes.size() != SignatureBytes(pattern)) return false;
    size_t at = 0;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') { ++i; continue; }
        if (!(pattern[i] == '?' && pattern[i + 1] == '?') &&
            bytes[at] != static_cast<uint8_t>((HexDigit(pattern[i]) << 4u) |
                                               HexDigit(pattern[i + 1])))
            return false;
        ++at;
        i += 2;
    }
    return true;
}
static_assert(SignatureBytes(VideoChoice) == 16);
static_assert(SignatureBytes(DisplayBuffer) == 20);
static_assert(SignatureBytes(SetterFixedPair) == 16);
static_assert(PackedCeiling == 0x43800000780ull);
} // namespace Loader::DemonsSoulsHdMobile
