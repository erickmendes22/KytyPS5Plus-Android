#ifndef KYTY_BUFFER_UPLOAD_POLICY_H_
#define KYTY_BUFFER_UPLOAD_POLICY_H_

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics::BufferUploadPolicy {
inline constexpr uint64_t MaxWindowBytes = 32ull * 1024 * 1024;

// A tracker clears complete dirty pages. Never put a page in two batches:
// clearing its first half would otherwise suppress the second half's upload.
[[nodiscard]] constexpr uint64_t WindowSize(uint64_t address, uint64_t remaining,
                                           uint64_t limit, uint64_t page_size) {
    if (page_size == 0 || limit < page_size || limit % page_size != 0) return 0;
    return std::min(remaining, limit - address % page_size);
}
} // namespace Libs::Graphics::BufferUploadPolicy
#endif
