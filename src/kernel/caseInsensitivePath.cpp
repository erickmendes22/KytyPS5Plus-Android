#include "kernel/caseInsensitivePath.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace Libs::LibKernel::FileSystem {

namespace {

// Directories whose listings are kept; a game touches a few hundred at most. Past this the cache
// starts over rather than tracking recency.
constexpr size_t kMaxCachedDirectories = 1024;

std::string ToLowerAscii(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
		return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	});
	return text;
}

} // namespace

std::filesystem::path CaseInsensitivePathResolver::Resolve(const std::filesystem::path& path) {
	std::error_code ec;
	if (std::filesystem::exists(path, ec)) {
		return path;
	}

	// Preserve unmatched components for the caller's ENOENT path.
	std::filesystem::path resolved =
	    path.has_root_path() ? path.root_path() : std::filesystem::path(".");
	bool matched = true;

	for (const auto& component: path.relative_path()) {
		if (component.empty()) {
			continue;
		}

		auto candidate = resolved / component;
		if (!matched || std::filesystem::exists(candidate, ec)) {
			resolved = std::move(candidate);
			continue;
		}

		if (auto entry = FindEntry(resolved, component.string())) {
			resolved /= *entry;
		} else {
			resolved = std::move(candidate);
			matched  = false;
		}
	}

	return resolved;
}

std::optional<std::string> CaseInsensitivePathResolver::FindEntry(const std::filesystem::path& dir,
                                                                  const std::string&           name) {
	const auto listing = GetListing(dir);
	if (listing == nullptr) {
		return std::nullopt;
	}
	const auto it = listing->name_by_lower_name.find(ToLowerAscii(name));
	if (it == listing->name_by_lower_name.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::shared_ptr<const CaseInsensitivePathResolver::Listing>
CaseInsensitivePathResolver::GetListing(const std::filesystem::path& dir) {
	std::error_code ec;
	const auto      mtime = std::filesystem::last_write_time(dir, ec);
	if (ec) {
		return nullptr;
	}

	const auto key = dir.string();
	{
		std::lock_guard lock(m_mutex);
		const auto      it = m_listings.find(key);
		if (it != m_listings.end() && it->second->mtime == mtime) {
			return it->second;
		}
	}

	// Scan outside the lock: other threads keep resolving paths in other directories meanwhile.
	auto listing = ReadListing(dir, mtime);
	if (listing == nullptr) {
		return nullptr;
	}

	std::lock_guard lock(m_mutex);
	if (m_listings.size() >= kMaxCachedDirectories && m_listings.find(key) == m_listings.end()) {
		m_listings.clear();
	}
	m_listings.insert_or_assign(key, listing);
	return listing;
}

std::shared_ptr<const CaseInsensitivePathResolver::Listing>
CaseInsensitivePathResolver::ReadListing(const std::filesystem::path&     dir,
                                         std::filesystem::file_time_type mtime) {
	std::unordered_map<std::string, std::string> names;
	std::error_code                              ec;
	for (std::filesystem::directory_iterator entry(dir, ec), end; !ec && entry != end;
	     entry.increment(ec)) {
		auto name = entry->path().filename().string();
		// The first entry in directory order wins, as the uncached lookup did.
		names.try_emplace(ToLowerAscii(name), std::move(name));
	}
	m_scans.fetch_add(1, std::memory_order_relaxed);
	if (ec) {
		return nullptr;
	}
	return std::make_shared<const Listing>(Listing {mtime, std::move(names)});
}

} // namespace Libs::LibKernel::FileSystem
