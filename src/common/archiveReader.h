#ifndef KYTY_COMMON_ARCHIVE_READER_H_
#define KYTY_COMMON_ARCHIVE_READER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

struct ArchiveDirEntry {
	std::string name;
	bool        is_file;
};

// Backends expose immutable metadata and offset-based reads. Cursor management, path routing,
// reader lifetime and the host-stack worker are shared by every archive format (archive.cpp).
// Members are archive-relative, '/'-separated and without a leading slash; "" is the root.
class ArchiveReader {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};

	virtual ~ArchiveReader() = default;

	virtual std::optional<Entry>         Find(std::string_view member) = 0;
	virtual std::vector<ArchiveDirEntry> List(std::string_view member) = 0;
	virtual uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) = 0;
};

} // namespace Common

#endif /* KYTY_COMMON_ARCHIVE_READER_H_ */
