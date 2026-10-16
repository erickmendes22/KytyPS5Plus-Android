// Case-insensitive guest path resolution on case-sensitive hosts.
//
// Games probe several asset folders for each file, so most lookups miss. A miss used to rescan the
// whole directory every time; on Android's FUSE storage one scan of Astro Bot's 12k-entry
// data/prein/bg/odx takes ~0.5 s, and thousands of misses stalled its first level load for 15+ min.
#include "kernel/caseInsensitivePath.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using Libs::LibKernel::FileSystem::CaseInsensitivePathResolver;

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::printf("FAILED: %s\n", text);
		g_failures++;
	}
}

void Touch(const std::filesystem::path& path) {
	std::ofstream(path) << "x";
}

// macOS volumes usually ignore case themselves; the host then finds every path directly.
bool HostIgnoresCase(const std::filesystem::path& dir) {
	Touch(dir / "case_probe");
	std::error_code ec;
	const bool ignores = std::filesystem::exists(dir / "CASE_PROBE", ec);
	std::filesystem::remove(dir / "case_probe", ec);
	return ignores;
}

class TemporaryDirectory {
public:
	TemporaryDirectory() {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = std::filesystem::temp_directory_path() /
		         ("kyty_case_path_" + std::to_string(stamp));
		std::filesystem::create_directories(m_path);
	}
	~TemporaryDirectory() {
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}
	TemporaryDirectory(const TemporaryDirectory&)            = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

	[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
};

void TestExactPathNeedsNoScan() {
	TemporaryDirectory          dir;
	CaseInsensitivePathResolver resolver;
	Touch(dir.Path() / "Asset.bin");

	Check(resolver.Resolve(dir.Path() / "Asset.bin") == dir.Path() / "Asset.bin",
	      "an exact path resolves to itself");
	Check(resolver.DirectoryScans() == 0, "an exact path is not resolved by scanning");
}

void TestCaseMismatchResolves() {
	TemporaryDirectory          dir;
	if (HostIgnoresCase(dir.Path())) {
		return;
	}
	CaseInsensitivePathResolver resolver;
	std::filesystem::create_directories(dir.Path() / "Data" / "Prein");
	Touch(dir.Path() / "Data" / "Prein" / "Bot.ODXB");

	Check(resolver.Resolve(dir.Path() / "data" / "prein" / "bot.odxb") ==
	          dir.Path() / "Data" / "Prein" / "Bot.ODXB",
	      "every component is matched ignoring case");
}

void TestMissKeepsUnmatchedComponents() {
	TemporaryDirectory          dir;
	if (HostIgnoresCase(dir.Path())) {
		return;
	}
	CaseInsensitivePathResolver resolver;
	std::filesystem::create_directories(dir.Path() / "Odx");

	Check(resolver.Resolve(dir.Path() / "odx" / "missing" / "file.bin") ==
	          dir.Path() / "Odx" / "missing" / "file.bin",
	      "a miss keeps the matched prefix and the caller's unmatched components");
}

void TestRepeatedMissesScanOnce() {
	TemporaryDirectory          dir;
	CaseInsensitivePathResolver resolver;
	for (int i = 0; i < 64; i++) {
		Touch(dir.Path() / ("entry_" + std::to_string(i)));
	}

	for (int i = 0; i < 100; i++) {
		(void)resolver.Resolve(dir.Path() / ("absent_" + std::to_string(i) + ".odxb"));
	}
	Check(resolver.DirectoryScans() == 1, "misses in an unchanged directory reuse one listing");
}

void TestNewEntryInvalidatesListing() {
	TemporaryDirectory          dir;
	if (HostIgnoresCase(dir.Path())) {
		return;
	}
	CaseInsensitivePathResolver resolver;
	Touch(dir.Path() / "first.sav");
	Check(resolver.Resolve(dir.Path() / "SECOND.SAV") == dir.Path() / "SECOND.SAV",
	      "a missing file stays unresolved");

	// Directory timestamps can be coarse on some hosts; make the change observable.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	Touch(dir.Path() / "Second.sav");
	std::filesystem::last_write_time(
	    dir.Path(), std::filesystem::last_write_time(dir.Path()) + std::chrono::seconds(1));

	Check(resolver.Resolve(dir.Path() / "SECOND.SAV") == dir.Path() / "Second.sav",
	      "a file created after the listing was cached is found");
	Check(resolver.DirectoryScans() == 2, "a changed directory is listed again");
}

} // namespace

int main(int, char**) {
	TestExactPathNeedsNoScan();
	TestCaseMismatchResolves();
	TestMissKeepsUnmatchedComponents();
	TestRepeatedMissesScanOnce();
	TestNewEntryInvalidatesListing();

	if (g_failures != 0) {
		std::printf("CaseInsensitivePathTests: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("CaseInsensitivePathTests: all cases passed\n");
	return EXIT_SUCCESS;
}
