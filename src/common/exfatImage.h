#ifndef KYTY_COMMON_EXFAT_IMAGE_H_
#define KYTY_COMMON_EXFAT_IMAGE_H_

#include "common/archiveReader.h"

#include <filesystem>
#include <memory>

namespace Common {

// Read-only exFAT volumes. The image is either a raw volume (boot sector at byte 0) or a disk
// with an MBR or GPT whose exFAT partition is used. Compressed images and TexFAT transactions
// are not supported.
[[nodiscard]] bool IsExfatImageFile(const std::filesystem::path& path);
[[nodiscard]] std::shared_ptr<ArchiveReader> OpenExfatImage(const std::filesystem::path& path);

} // namespace Common

#endif /* KYTY_COMMON_EXFAT_IMAGE_H_ */
