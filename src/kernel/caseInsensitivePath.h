#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_CASEINSENSITIVEPATH_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_CASEINSENSITIVEPATH_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Libs::LibKernel::FileSystem {

// Resolves guest paths case-insensitively on case-sensitive hosts (PS5 paths ignore case).
//
// Games probe several asset folders for each file, so most lookups miss, and a miss has to look
// at every entry of the directory. Listings are therefore cached per directory and reused while
// the directory's modification time is unchanged: on Android's FUSE storage a single scan of a
// 12k-entry asset folder takes ~0.5 s, and rescanning on every miss stalled level loads.
class CaseInsensitivePathResolver {
public:
	CaseInsensitivePathResolver() = default;
	~CaseInsensitivePathResolver() = default;

	CaseInsensitivePathResolver(const CaseInsensitivePathResolver&)            = delete;
	CaseInsensitivePathResolver& operator=(const CaseInsensitivePathResolver&) = delete;

	// Returns the host path whose components match `path` ignoring case. Components that match
	// nothing are kept as given (with the matched prefix), so the caller's open fails with ENOENT.
	[[nodiscard]] std::filesystem::path Resolve(const std::filesystem::path& path);

	// Number of directory listings read so far (tests and diagnostics).
	[[nodiscard]] uint64_t DirectoryScans() const { return m_scans.load(std::memory_order_relaxed); }

private:
	struct Listing {
		std::filesystem::file_time_type              mtime;
		std::unordered_map<std::string, std::string> name_by_lower_name;
	};

	[[nodiscard]] std::optional<std::string> FindEntry(const std::filesystem::path& dir,
	                                                   const std::string&           name);
	[[nodiscard]] std::shared_ptr<const Listing> GetListing(const std::filesystem::path& dir);
	[[nodiscard]] std::shared_ptr<const Listing> ReadListing(const std::filesystem::path&     dir,
	                                                         std::filesystem::file_time_type mtime);

	std::mutex                                                       m_mutex;
	std::unordered_map<std::string, std::shared_ptr<const Listing>> m_listings;
	std::atomic<uint64_t>                                            m_scans {0};
};

} // namespace Libs::LibKernel::FileSystem

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_CASEINSENSITIVEPATH_H_ */
