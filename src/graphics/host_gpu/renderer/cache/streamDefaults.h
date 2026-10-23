#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_STREAMDEFAULTS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_STREAMDEFAULTS_H_

#include <cstdint>

// Start values of the stream-path and SRT memo knobs from their environment strings (nullptr =
// unset). Unset means on with the measured values on Android (Mesa Turnip, the validated target)
// and off elsewhere; an explicit value wins everywhere (=0 opts out). Measured on PPSA28855
// (Totally Reliable Delivery Service): 26.9 -> 30.3 fps sustained, 5-min walk 35.3 fps; Astro
// pixels at the noise floor (opus-g1 c83b1400 / fe2a0b52).
namespace Libs::Graphics::StreamDefaults {

#if defined(__ANDROID__)
inline constexpr bool kAndroid = true;
#else
inline constexpr bool kAndroid = false;
#endif

// KYTY_STREAM_PROMOTE default on Android: unchanged copies before a small read is promoted.
inline constexpr int64_t kAndroidPromoteThreshold = 2;

enum class MemoMode { Off, On, Auto };

namespace Detail {

[[nodiscard]] constexpr bool Equals(const char* value, const char* expected) {
	for (; *value != '\0' && *value == *expected; ++value, ++expected) {}
	return *value == *expected;
}

// Leading decimal digits (strtoll-like; no sign = a negative value or text reads as 0).
[[nodiscard]] constexpr int64_t LeadingNumber(const char* value) {
	int64_t n = 0;
	for (; *value >= '0' && *value <= '9'; ++value) {
		n = n * 10 + (*value - '0');
	}
	return n;
}

} // namespace Detail

// KYTY_STREAM_PROMOTE=<n>: 0 = off.
[[nodiscard]] constexpr int64_t PromoteThreshold(const char* value, bool android) {
	if (value == nullptr) return android ? kAndroidPromoteThreshold : 0;
	return Detail::LeadingNumber(value);
}

// KYTY_STREAM_CACHED=1: a host-cached stream buffer for small CPU-modified reads.
[[nodiscard]] constexpr bool CachedStream(const char* value, bool android) {
	if (value == nullptr) return android;
	return Detail::Equals(value, "1");
}

// KYTY_SRT_MEMO=0|1|auto: auto = the memo only while the walk fan-out is off.
[[nodiscard]] constexpr MemoMode SrtMemo(const char* value, bool android) {
	if (value == nullptr) return android ? MemoMode::Auto : MemoMode::Off;
	if (Detail::Equals(value, "auto")) return MemoMode::Auto;
	return value[0] == '1' ? MemoMode::On : MemoMode::Off;
}

} // namespace Libs::Graphics::StreamDefaults

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_STREAMDEFAULTS_H_
