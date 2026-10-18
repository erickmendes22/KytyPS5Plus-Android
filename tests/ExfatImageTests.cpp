#include "common/exfatImage.h"
#include "ExfatImageFixture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ExfatFixture::Entry;
using Common::ArchiveReader;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ExfatImageTests: failed: %s\n", text);
		std::abort();
	}
}

class TempDirectory {
public:
	TempDirectory() {
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path            = std::filesystem::temp_directory_path() / ("kyty_exfat_test_" + std::to_string(unique));
		Check(std::filesystem::create_directories(m_path), "create temporary directory");
	}

	~TempDirectory() {
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}

	[[nodiscard]] std::filesystem::path File(const char* name) const { return m_path / name; }

	TempDirectory(const TempDirectory&)            = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

private:
	std::filesystem::path m_path;
};

void WriteImage(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	Check(out.good(), "write image");
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i < size; ++i) {
		bytes[i] = static_cast<uint8_t>(i * 7 + seed + (i >> 12));
	}
	return bytes;
}

std::vector<uint8_t> Slice(const std::vector<uint8_t>& bytes, size_t offset, size_t size) {
	if (offset >= bytes.size()) {
		return {};
	}
	const auto end = std::min(bytes.size(), offset + size);
	return {bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(end)};
}

std::optional<ArchiveReader::Entry> Lookup(ArchiveReader& reader, std::string_view member) {
	return reader.Find(member);
}

// Reads `size` bytes at `offset`, looping like ArchiveFile does.
std::vector<uint8_t> ReadRange(ArchiveReader& reader, const ArchiveReader::Entry& entry, uint64_t offset, uint32_t size) {
	std::vector<uint8_t> out(size);
	const auto           got = reader.Read(entry.id, offset, size, out.data());
	out.resize(static_cast<size_t>(got));
	return out;
}

std::vector<uint8_t> ReadWhole(ArchiveReader& reader, std::string_view member) {
	const auto entry = Lookup(reader, member);
	Check(entry.has_value() && entry->is_file, "file exists");
	return ReadRange(reader, *entry, 0, static_cast<uint32_t>(entry->size));
}

std::shared_ptr<ArchiveReader> OpenVolume(const TempDirectory& dir, const char* name, const std::vector<uint8_t>& bytes) {
	const auto path = dir.File(name);
	WriteImage(path, bytes);
	auto reader = Common::OpenExfatImage(path);
	Check(reader != nullptr, "image opens");
	return reader;
}

std::vector<Entry> SampleTree() {
	const std::string long_name = std::string(160, 'a') + ".bin";
	std::vector<Entry> entries;
	entries.push_back({.path = "eboot.bin", .data = Pattern(5000, 1)});
	entries.push_back({.path = "sce_sys", .directory = true});
	entries.push_back({.path = "sce_sys/param.json", .data = {'{', '}'}, .fragmented = true});
	entries.push_back({.path = "assets", .directory = true, .fragmented = true});
	entries.push_back({.path = "assets/Sub", .directory = true});
	entries.push_back({.path = "assets/Sub/data.bin", .data = Pattern(3 * 4096 + 77, 2), .fragmented = true});
	entries.push_back({.path = "assets/Sub/" + long_name, .data = Pattern(10, 3)});
	entries.push_back({.path = "assets/Sub/asset-\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.bin", .data = Pattern(100, 4)});
	entries.push_back({.path = "empty.bin"});
	return entries;
}

void TestNestedFilesReadBack(const TempDirectory& dir) {
	const auto tree   = SampleTree();
	auto       reader = OpenVolume(dir, "raw.exfat", ExfatFixture::BuildExfatVolume(tree));

	Check(ReadWhole(*reader, "eboot.bin") == Pattern(5000, 1), "contiguous root file reads back");
	Check(ReadWhole(*reader, "sce_sys/param.json") == std::vector<uint8_t>({'{', '}'}), "fragmented nested file");
	Check(ReadWhole(*reader, "assets/Sub/data.bin") == Pattern(3 * 4096 + 77, 2), "multi-cluster fragmented file");
	Check(ReadWhole(*reader, "assets/Sub/" + std::string(160, 'a') + ".bin") == Pattern(10, 3), "long name");

	const auto info = Lookup(*reader, "assets/Sub/data.bin");
	Check(info.has_value() && info->is_file && info->size == 3 * 4096 + 77, "file size from stream extension");

	const auto dir_info = Lookup(*reader, "assets/Sub");
	Check(dir_info.has_value() && !dir_info->is_file, "directory is not a file");
}

void TestOffsetReadsCrossClusters(const TempDirectory& dir) {
	const auto tree   = SampleTree();
	auto       reader = OpenVolume(dir, "offsets.exfat", ExfatFixture::BuildExfatVolume(tree));

	const auto contiguous = Pattern(5000, 1);
	const auto entry      = Lookup(*reader, "eboot.bin");
	Check(entry.has_value(), "eboot found");
	Check(ReadRange(*reader, *entry, 4090, 20) == Slice(contiguous, 4090, 20), "contiguous read across cluster edge");

	const auto chained = Pattern(3 * 4096 + 77, 2);
	const auto chain   = Lookup(*reader, "assets/Sub/data.bin");
	Check(chain.has_value(), "data.bin found");
	Check(ReadRange(*reader, *chain, 4000, 300) == Slice(chained, 4000, 300), "chained read across cluster edge");
	Check(ReadRange(*reader, *chain, 8190, 4096) == Slice(chained, 8190, 4096), "chained read to EOF is clipped");
	Check(ReadRange(*reader, *chain, 3 * 4096 + 77, 16).empty(), "read at EOF returns nothing");
	Check(ReadRange(*reader, *chain, 3 * 4096 + 1000, 16).empty(), "read past EOF returns nothing");
}

void TestNameLookupIsCaseInsensitiveForAscii(const TempDirectory& dir) {
	const auto tree   = SampleTree();
	auto       reader = OpenVolume(dir, "names.exfat", ExfatFixture::BuildExfatVolume(tree));

	Check(Lookup(*reader, "ASSETS/sub/DATA.BIN").has_value(), "ASCII folding applies to every component");
	Check(Lookup(*reader, "Sce_Sys/Param.Json").has_value(), "mixed case component");
	Check(Lookup(*reader, "assets/Sub/asset-\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.BIN").has_value(),
	      "ASCII part folds, non-ASCII matches exactly");
	Check(!Lookup(*reader, "assets/Sub/ASSET-\xc3\x89-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.bin").has_value(),
	      "non-ASCII letters do not fold");
	Check(!Lookup(*reader, "assets/Sub/missing.bin").has_value(), "missing file");
	Check(!Lookup(*reader, "sce_sys/param.json/deeper").has_value(), "file used as directory");
	Check(Lookup(*reader, "eboot.bin/").has_value(), "trailing slash is ignored");
	Check(Lookup(*reader, "").has_value(), "empty member is the root");
}

void TestListingReturnsChildrenWithOriginalNames(const TempDirectory& dir) {
	const auto tree   = SampleTree();
	auto       reader = OpenVolume(dir, "list.exfat", ExfatFixture::BuildExfatVolume(tree));

	const auto root = reader->List("");
	std::vector<std::string> names;
	for (const auto& child : root) {
		names.push_back(child.name);
	}
	Check(names.size() == 4, "root has four children");
	Check(std::find(names.begin(), names.end(), "eboot.bin") != names.end(), "root lists eboot.bin");
	Check(std::find(names.begin(), names.end(), "assets") != names.end(), "root lists assets");

	const auto sub = reader->List("assets/Sub");
	Check(sub.size() == 3, "Sub lists three files");
	Check(reader->List("eboot.bin").empty(), "a file lists nothing");
	Check(reader->List("missing").empty(), "missing directory lists nothing");
}

void TestManyEntriesSpanDirectoryClusters(const TempDirectory& dir) {
	std::vector<Entry> entries;
	entries.push_back({.path = "dir", .directory = true});
	for (int i = 0; i < 300; ++i) {
		entries.push_back({.path = "dir/file-" + std::to_string(1000 + i) + ".bin", .data = Pattern(i, 5)});
	}
	auto reader = OpenVolume(dir, "many.exfat", ExfatFixture::BuildExfatVolume(entries));

	Check(reader->List("dir").size() == 300, "all entries across several directory clusters");
	Check(ReadWhole(*reader, "dir/FILE-1299.BIN") == Pattern(299, 5), "last entry in the chain");
	Check(ReadWhole(*reader, "dir/file-1000.bin") == Pattern(0, 5), "zero-length file");
}

void TestEmptyAndSkippedEntries(const TempDirectory& dir) {
	std::vector<Entry> entries;
	entries.push_back({.path = "ok.bin", .data = Pattern(64, 9)});
	entries.push_back({.path = "broken.bin", .data = Pattern(64, 9), .bad_checksum = true});
	auto reader = OpenVolume(dir, "checksum.exfat", ExfatFixture::BuildExfatVolume(entries));

	Check(ReadWhole(*reader, "ok.bin") == Pattern(64, 9), "good entry set is read");
	Check(!Lookup(*reader, "broken.bin").has_value(), "entry set with a bad checksum is skipped");
}

void TestMbrPartitionedImage(const TempDirectory& dir) {
	const auto tree   = SampleTree();
	const auto disk   = ExfatFixture::WrapInMbr(ExfatFixture::BuildExfatVolume(tree));
	auto       reader = OpenVolume(dir, "disk-mbr.img", disk);

	Check(ReadWhole(*reader, "assets/Sub/data.bin") == Pattern(3 * 4096 + 77, 2), "file inside MBR partition");
	Check(Common::IsExfatImageFile(dir.File("disk-mbr.img")), ".img with MBR partition is accepted");
}

void TestGptPartitionedImage(const TempDirectory& dir) {
	const auto tree   = SampleTree();
	const auto disk   = ExfatFixture::WrapInGpt(ExfatFixture::BuildExfatVolume(tree));
	auto       reader = OpenVolume(dir, "disk-gpt.img", disk);

	Check(ReadWhole(*reader, "sce_sys/param.json") == std::vector<uint8_t>({'{', '}'}), "file inside GPT partition");
}

void TestRejectsNonExfatImages(const TempDirectory& dir) {
	const auto zeros = dir.File("zeros.exfat");
	WriteImage(zeros, std::vector<uint8_t>(1 << 20, 0));
	Check(Common::OpenExfatImage(zeros) == nullptr, "zero-filled file is not exFAT");
	Check(!Common::IsExfatImageFile(zeros), "zero-filled .img is not exFAT");

	auto volume = ExfatFixture::BuildExfatVolume(SampleTree());
	volume[20]  = 0x01; // inside the MustBeZero region (bytes 11..63)
	const auto dirty = dir.File("dirty.img");
	WriteImage(dirty, volume);
	Check(Common::OpenExfatImage(dirty) == nullptr, "MustBeZero violation rejected");

	volume          = ExfatFixture::BuildExfatVolume(SampleTree());
	volume[510]     = 0x00;
	const auto nosig = dir.File("nosig.img");
	WriteImage(nosig, volume);
	Check(Common::OpenExfatImage(nosig) == nullptr, "missing boot signature rejected");

	Check(Common::OpenExfatImage(dir.File("does-not-exist.exfat")) == nullptr, "missing file");
}

void TestIsExfatImageFileForRawVolume(const TempDirectory& dir) {
	const auto raw = dir.File("raw.img");
	WriteImage(raw, ExfatFixture::BuildExfatVolume(SampleTree()));
	Check(Common::IsExfatImageFile(raw), "raw exFAT boot sector is accepted");
}

} // namespace

int main() {
	const TempDirectory dir;
	TestNestedFilesReadBack(dir);
	TestOffsetReadsCrossClusters(dir);
	TestNameLookupIsCaseInsensitiveForAscii(dir);
	TestListingReturnsChildrenWithOriginalNames(dir);
	TestManyEntriesSpanDirectoryClusters(dir);
	TestEmptyAndSkippedEntries(dir);
	TestMbrPartitionedImage(dir);
	TestGptPartitionedImage(dir);
	TestRejectsNonExfatImages(dir);
	TestIsExfatImageFileForRawVolume(dir);
	std::printf("ExfatImageTests: all passed\n");
	return 0;
}
