#ifndef GRAPHICS_SHADER_LOOP_GUARD_H
#define GRAPHICS_SHADER_LOOP_GUARD_H

// KYTY_LOOP_GUARD=<hex hash prefixes, ':' or ',' separated> (diagnostic and safety net, off by
// default): in the listed shaders, every structured loop header counts one iteration in a
// per-invocation counter, and each loop's exit branch is taken once the counter passes
// KYTY_LOOP_GUARD_N (default 65536). The guest code would loop forever on the same data (a
// cyclic linked list in Astro Bot's CS 657ad046, research/NEXT.md), which a Turnip GPU turns into
// a 10 s kgsl hang kill per frame; guarded, the shader finishes with wrong results instead.
// The first time an invocation trips, it records itself in a host-visible log (when the renderer
// set a log address before the shader compiled): dword 0 counts trips, entries of kEntryDwords
// follow: guest shader hash low/high, loop header block id, iterations, then the first kPhis
// 32-bit loop-carried values (the header's U32 phis) of the low lane half. Logged as
// "[loop-guard]" (BufferCache::PollLoopGuardLog).

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::LoopGuard {

inline constexpr uint32_t kHeaderDwords = 4;
inline constexpr uint32_t kPhis         = 8;
inline constexpr uint32_t kEntryDwords  = 4 + kPhis;
inline constexpr uint32_t kMaxEntries   = 64;
inline constexpr uint32_t kLogBytes     = (kHeaderDwords + kEntryDwords * kMaxEntries) * 4u;
inline constexpr uint32_t kDefaultLimit = 65536;

// True when `spec` lists a hex prefix of the 16-digit hash (case-insensitive). Tokens are
// separated by ':' or ','; an optional "0x" prefix is ignored; empty tokens never match.
[[nodiscard]] inline bool Matches(const char* spec, uint64_t hash) {
	if (spec == nullptr) {
		return false;
	}
	char digits[17];
	std::snprintf(digits, sizeof(digits), "%016" PRIx64, hash);
	const char* token = spec;
	while (*token != '\0') {
		const char* end = token;
		while (*end != '\0' && *end != ':' && *end != ',') {
			end++;
		}
		const char* begin = token;
		if (end - begin >= 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
			begin += 2;
		}
		const auto length = static_cast<size_t>(end - begin);
		bool       match  = length > 0 && length <= 16;
		for (size_t i = 0; match && i < length; i++) {
			const char c = begin[i];
			const char l = (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
			match        = l == digits[i];
		}
		if (match) {
			return true;
		}
		token = *end == '\0' ? end : end + 1;
	}
	return false;
}

// Iterations an invocation may run through guarded loop headers: KYTY_LOOP_GUARD_N, at least 1.
[[nodiscard]] inline uint32_t ParseLimit(const char* value) {
	if (value == nullptr || *value == '\0') {
		return kDefaultLimit;
	}
	const auto parsed = std::strtoull(value, nullptr, 0);
	return parsed == 0 ? 1u : parsed > UINT32_MAX - 1u ? UINT32_MAX - 1u : static_cast<uint32_t>(parsed);
}

[[nodiscard]] inline const char* Spec() {
	static const char* spec = [] {
		const char* value = std::getenv("KYTY_LOOP_GUARD");
		return value != nullptr && *value != '\0' ? value : nullptr;
	}();
	return spec;
}
[[nodiscard]] inline bool Enabled() {
	return Spec() != nullptr;
}
[[nodiscard]] inline bool Guards(uint64_t hash) {
	return Matches(Spec(), hash);
}
[[nodiscard]] inline uint32_t Limit() {
	static const uint32_t limit = ParseLimit(std::getenv("KYTY_LOOP_GUARD_N"));
	return limit;
}

namespace Detail {
inline std::atomic<uint64_t> g_log_address {0};
} // namespace Detail
// Set once by the renderer before guarded shaders compile; 0: loops are guarded but not logged.
inline void SetLogAddress(uint64_t address) {
	Detail::g_log_address.store(address, std::memory_order_release);
}
[[nodiscard]] inline uint64_t LogAddress() {
	return Detail::g_log_address.load(std::memory_order_acquire);
}

} // namespace Libs::Graphics::LoopGuard

#endif // GRAPHICS_SHADER_LOOP_GUARD_H
