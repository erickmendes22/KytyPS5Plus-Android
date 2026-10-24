#ifndef KYTY_LOADER_MFGT_ARENA_FIX_H_
#define KYTY_LOADER_MFGT_ARENA_FIX_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// PPSA24156's bundled allocator ignores a failed fixed batch-map and then clears
// the unmapped pointer. On ARM64 its third arena folds outside the owned guest VA.
// Relocate all three bases AND all pointer classifiers together; never patch only
// the crashing initializer. The guest arena sizes and pointer representation stay intact.
namespace Loader::MfgtArenaFix {

inline constexpr uint64_t kEntry = 0xfef5b0;
inline constexpr size_t kTextSize = 0x3e4548c;
inline constexpr uint64_t kTextHash = 0x4f0ea18498883c85;
inline constexpr uint64_t kOldBase = 0xe400000000;
inline constexpr uint64_t kNewBase = 0xc400000000;
inline constexpr uint64_t kArenaSize = 0x800000000;

struct Site {
    size_t offset; // immediate RVA in the executable PT_LOAD at RVA zero
    uint8_t rex, opcode;
    uint64_t before, after;
};
inline constexpr std::array<Site, 27> kSites {{
    {0x609, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0x9e9, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xb0b21, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xb1349, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xb507d, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xc4d79, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xcc8f7, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xcca8e, 0x49, 0xbf, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xcccb1, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xd920e, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0x2a4f79, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0x3fda17, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0x3fdd89, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xade422, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xc9b92b, 0x48, 0xbe, 0x000000f400000000ull, 0x000000d400000000ull},
    {0xc9bbcb, 0x48, 0xbe, 0x000000ec00000000ull, 0x000000cc00000000ull},
    {0xc9bc5d, 0x48, 0xb8, 0x000000e400000000ull, 0x000000c400000000ull},
    {0xfb5f13, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfb8536, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfb9c03, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfbb9ff, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfbc75d, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfbd209, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfbeb7f, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfc1b4a, 0x49, 0xbd, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0xfc5e73, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
    {0x3dfb2d2, 0x48, 0xb8, 0xffffff1c00000000ull, 0xffffff3c00000000ull},
}};

enum class Result { NotApplicable, UnsupportedBuild, Applied };

[[nodiscard]] inline uint64_t Fingerprint(std::span<const uint8_t> text) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const auto byte: text) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}

namespace Detail {

[[nodiscard]] inline uint64_t Read(std::span<const uint8_t> text, size_t offset) noexcept {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= uint64_t{text[offset + i]} << (i * 8);
    return value;
}

inline void Write(std::span<uint8_t> text, size_t offset, uint64_t value) noexcept {
    for (size_t i = 0; i < 8; ++i) text[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

// Validate every site before changing even one byte. This is kept separate so the
// transactional byte plan is testable with a synthetic image; Apply also requires
// the full pristine executable fingerprint before this function can run in the loader.
[[nodiscard]] inline bool ApplySites(std::span<uint8_t> text) noexcept {
    for (const auto& site: kSites) {
        if (site.offset < 2 || site.offset > text.size() || text.size() - site.offset < 8 ||
            text[site.offset - 2] != site.rex || text[site.offset - 1] != site.opcode ||
            Read(text, site.offset) != site.before) return false;
    }
    for (const auto& site: kSites) Write(text, site.offset, site.after);
    return true;
}

} // namespace Detail

[[nodiscard]] inline Result Apply(std::string_view title_id, bool main_executable,
                                  uint64_t entry, uint64_t segment_rva,
                                  std::span<uint8_t> pristine_text) noexcept {
    if (title_id != "PPSA24156" || !main_executable || segment_rva != 0)
        return Result::NotApplicable;
    if (entry != kEntry || pristine_text.size() != kTextSize ||
        Fingerprint(pristine_text) != kTextHash || !Detail::ApplySites(pristine_text))
        return Result::UnsupportedBuild;
    return Result::Applied;
}

} // namespace Loader::MfgtArenaFix

#endif
