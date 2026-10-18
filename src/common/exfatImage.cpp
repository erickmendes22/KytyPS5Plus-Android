#include "common/exfatImage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// Read-only exFAT (Microsoft exFAT specification). Standard library only: the host unit test
// compiles this file without the generated cmake_config.h.

namespace Common {
namespace {

// MBR and GPT addresses are always in 512-byte logical blocks, whatever the volume uses.
constexpr uint64_t SectorBytes        = 512;
constexpr size_t   BootSectorBytes    = 512;
constexpr uint64_t DirEntryBytes      = 32;
constexpr uint64_t FirstClusterNumber = 2;
constexpr uint32_t MinEndOfChain      = 0xFFFFFFF8u;
constexpr uint64_t MinFatOffset       = 24;
constexpr uint8_t  EntryEndOfDirectory = 0x00;
constexpr uint8_t  EntryInUse          = 0x80;
constexpr uint8_t  EntryFile           = 0x85;
constexpr uint8_t  EntryStream         = 0xC0;
constexpr uint8_t  EntryName           = 0xC1;
constexpr uint8_t  StreamNoFatChain    = 0x02;
constexpr uint16_t AttributeDirectory  = 0x0010;
constexpr uint16_t VolumeFlagActiveFat = 0x0001;
constexpr size_t   NameUnitsPerEntry   = 15;
constexpr size_t   MaxNameUnits        = 255;
constexpr uint64_t MaxDirectoryBytes   = uint64_t{1} << 25; // 2^20 entries
constexpr size_t   MbrPartitionCount   = 4;
constexpr size_t   MbrTableOffset      = 446;
constexpr size_t   MbrEntryBytes       = 16;
constexpr uint32_t GptMaxEntries       = 128;
constexpr size_t   GptEntryBytes       = 128;
constexpr size_t   CursorCacheLimit    = 64;

uint16_t LoadU16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t LoadU32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t LoadU64(const uint8_t* p) {
	return static_cast<uint64_t>(LoadU32(p)) | (static_cast<uint64_t>(LoadU32(p + 4)) << 32);
}

// Reads exactly `size` bytes at `offset`. False on a short read or a seek past the end.
bool ReadBytes(std::ifstream& file, uint64_t offset, void* data, uint64_t size) {
	if (size == 0) {
		return true;
	}
	file.clear();
	file.seekg(static_cast<std::streamoff>(offset));
	file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
	return file.gcount() == static_cast<std::streamsize>(size);
}

std::optional<uint64_t> FileSize(std::ifstream& file) {
	file.seekg(0, std::ios::end);
	const auto end = static_cast<std::streamoff>(file.tellg());
	if (end < 0) {
		return {};
	}
	return static_cast<uint64_t>(end);
}

// Resolved location of one exFAT volume inside the image file.
struct Volume {
	uint64_t cluster_bytes = 0;
	uint64_t fat_byte      = 0; // the active FAT
	uint64_t heap_byte     = 0; // cluster 2
	uint32_t cluster_count = 0;
	uint32_t root_cluster  = 0;
};

// Validates the main boot sector: signature, MustBeZero field, geometry and root cluster.
std::optional<Volume> ParseBootSector(const uint8_t* sector, uint64_t base) {
	if (std::memcmp(sector + 3, "EXFAT   ", 8) != 0) {
		return {};
	}
	for (size_t i = 11; i < 64; ++i) {
		if (sector[i] != 0) {
			return {};
		}
	}
	if (sector[510] != 0x55 || sector[511] != 0xAA) {
		return {};
	}

	const uint64_t bytes_shift   = sector[108];
	const uint64_t cluster_shift = sector[109];
	const uint64_t fat_count     = sector[110];
	if (bytes_shift < 9 || bytes_shift > 12 || bytes_shift + cluster_shift > 25) {
		return {};
	}
	if (fat_count < 1 || fat_count > 2) {
		return {};
	}

	const uint64_t volume_length = LoadU64(sector + 72);
	const uint64_t fat_offset    = LoadU32(sector + 80);
	const uint64_t fat_length    = LoadU32(sector + 84);
	const uint64_t heap_offset   = LoadU32(sector + 88);
	const uint64_t cluster_count = LoadU32(sector + 92);
	const uint64_t root_cluster  = LoadU32(sector + 96);
	const uint16_t flags         = LoadU16(sector + 106);

	if (fat_offset < MinFatOffset || fat_length == 0 || heap_offset < fat_offset + fat_count * fat_length) {
		return {};
	}
	// Every cluster number, including the two reserved entries, needs a 4-byte FAT slot.
	if ((fat_length << bytes_shift) < (cluster_count + FirstClusterNumber) * 4) {
		return {};
	}
	if (cluster_count == 0 || heap_offset + (cluster_count << cluster_shift) > volume_length) {
		return {};
	}
	if (root_cluster < FirstClusterNumber || root_cluster >= FirstClusterNumber + cluster_count) {
		return {};
	}

	const uint64_t active_fat = (fat_count == 2 && (flags & VolumeFlagActiveFat) != 0) ? 1 : 0;
	Volume         volume;
	volume.cluster_bytes = uint64_t{1} << (bytes_shift + cluster_shift);
	volume.fat_byte      = base + ((fat_offset + active_fat * fat_length) << bytes_shift);
	volume.heap_byte     = base + (heap_offset << bytes_shift);
	volume.cluster_count = static_cast<uint32_t>(cluster_count);
	volume.root_cluster  = static_cast<uint32_t>(root_cluster);
	return volume;
}

std::optional<Volume> ProbeVolume(std::ifstream& file, uint64_t base, uint64_t file_size) {
	if (base > file_size || file_size - base < SectorBytes) {
		return {};
	}
	std::array<uint8_t, BootSectorBytes> sector {};
	if (!ReadBytes(file, base, sector.data(), sector.size())) {
		return {};
	}
	return ParseBootSector(sector.data(), base);
}

// A raw volume at byte 0, or the first exFAT partition of an MBR or GPT disk.
std::optional<Volume> FindVolume(std::ifstream& file, uint64_t file_size) {
	if (auto volume = ProbeVolume(file, 0, file_size)) {
		return volume;
	}

	std::array<uint8_t, BootSectorBytes> sector {};
	if (!ReadBytes(file, 0, sector.data(), sector.size())) {
		return {};
	}
	if (sector[510] != 0x55 || sector[511] != 0xAA) {
		return {};
	}
	for (size_t i = 0; i < MbrPartitionCount; ++i) {
		const uint8_t* entry = sector.data() + MbrTableOffset + i * MbrEntryBytes;
		const uint64_t lba   = LoadU32(entry + 8);
		if (entry[4] != 0 && lba != 0) {
			if (auto volume = ProbeVolume(file, lba * SectorBytes, file_size)) {
				return volume;
			}
		}
	}

	// GPT. The protective MBR entry (type 0xEE) points at the header in LBA 1.
	if (!ReadBytes(file, SectorBytes, sector.data(), sector.size()) ||
	    std::memcmp(sector.data(), "EFI PART", 8) != 0) {
		return {};
	}
	const uint64_t entries_lba  = LoadU64(sector.data() + 72);
	const uint32_t entry_count  = std::min(LoadU32(sector.data() + 80), GptMaxEntries);
	const uint32_t entry_bytes  = LoadU32(sector.data() + 84);
	if (entry_bytes < GptEntryBytes || entry_bytes > 512) {
		return {};
	}
	for (uint32_t i = 0; i < entry_count; ++i) {
		std::array<uint8_t, GptEntryBytes> entry {};
		const uint64_t offset = entries_lba * SectorBytes + uint64_t{i} * entry_bytes;
		if (!ReadBytes(file, offset, entry.data(), entry.size())) {
			break;
		}
		const uint64_t first_lba = LoadU64(entry.data() + 32);
		if (first_lba != 0) {
			if (auto volume = ProbeVolume(file, first_lba * SectorBytes, file_size)) {
				return volume;
			}
		}
	}
	return {};
}

// exFAT matches names case-insensitively for ASCII only. Other characters must match exactly.
// UTF-8 continuation bytes are all >= 0x80, so folding ASCII bytes here is the same rule.
std::string FoldName(std::string_view name) {
	std::string folded(name);
	for (auto& c : folded) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return folded;
}

void AppendUtf8(std::string& out, uint32_t code_point) {
	if (code_point < 0x80) {
		out += static_cast<char>(code_point);
	} else if (code_point < 0x800) {
		out += static_cast<char>(0xC0 | (code_point >> 6));
		out += static_cast<char>(0x80 | (code_point & 0x3F));
	} else if (code_point < 0x10000) {
		out += static_cast<char>(0xE0 | (code_point >> 12));
		out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (code_point & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (code_point >> 18));
		out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (code_point & 0x3F));
	}
}

// Unpaired surrogates become U+FFFD.
std::string Utf16ToUtf8(const std::u16string& units) {
	std::string out;
	out.reserve(units.size());
	for (size_t i = 0; i < units.size(); ++i) {
		uint32_t unit = units[i];
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
		    units[i + 1] <= 0xDFFF) {
			const uint32_t low = units[i + 1];
			unit               = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
			++i;
		} else if (unit >= 0xD800 && unit <= 0xDFFF) {
			unit = 0xFFFD;
		}
		AppendUtf8(out, unit);
	}
	return out;
}

// SetChecksum over an entry set. Bytes 2 and 3 of the primary entry hold the checksum itself.
uint16_t EntrySetChecksum(const uint8_t* set, size_t size) {
	uint16_t sum = 0;
	for (size_t i = 0; i < size; ++i) {
		if (i == 2 || i == 3) {
			continue;
		}
		const auto rotated = static_cast<uint16_t>((sum << 15) | (sum >> 1));
		sum                = static_cast<uint16_t>(rotated + set[i]);
	}
	return sum;
}

std::vector<std::string_view> SplitPath(std::string_view path) {
	std::vector<std::string_view> parts;
	while (!path.empty()) {
		const auto separator = path.find('/');
		const auto part      = path.substr(0, separator);
		if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		path.remove_prefix(separator == std::string_view::npos ? path.size() : separator + 1);
	}
	return parts;
}

// Identity of a node. Equal streams share one id, so repeated lookups do not grow the table.
struct Node {
	uint32_t first_cluster;
	uint64_t size;
	bool     no_fat_chain;
	bool     is_file;
};

struct Child {
	std::string name;
	bool        is_file;
	uint64_t    id;
};

struct Directory {
	std::vector<Child>                      children;
	std::unordered_map<std::string, size_t> by_name; // folded name -> index into children
};

// Last position reached in a FAT chain, so sequential reads do not walk from the start.
struct Cursor {
	uint64_t index;
	uint32_t cluster;
};

class ExfatReader final: public ArchiveReader {
public:
	ExfatReader(std::ifstream&& file, const Volume& volume): m_file(std::move(file)), m_volume(volume) {}

	ExfatReader(const ExfatReader&)            = delete;
	ExfatReader& operator=(const ExfatReader&) = delete;

	// Interns the root directory. False if its FAT chain is unreadable.
	bool Init() {
		std::lock_guard lock(m_mutex);
		const uint64_t length = ChainLength(m_volume.root_cluster) * m_volume.cluster_bytes;
		if (length == 0) {
			return false;
		}
		m_root = InternNode(m_volume.root_cluster, length, false, false);
		return true;
	}

	std::optional<Entry> Find(std::string_view member) override {
		std::lock_guard lock(m_mutex);
		return Resolve(member);
	}

	std::vector<ArchiveDirEntry> List(std::string_view member) override {
		std::lock_guard              lock(m_mutex);
		std::vector<ArchiveDirEntry> result;
		const auto                   entry = Resolve(member);
		if (!entry || entry->is_file) {
			return result;
		}
		const Directory* dir = LoadDirectory(entry->id);
		if (dir == nullptr) {
			return result;
		}
		result.reserve(dir->children.size());
		for (const auto& child : dir->children) {
			result.push_back({child.name, child.is_file});
		}
		return result;
	}

	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) override {
		std::lock_guard lock(m_mutex);
		if (id >= m_nodes.size() || !m_nodes[id].is_file) {
			return 0;
		}
		const Node node = m_nodes[id];
		return ReadRange(id, node, offset, size, static_cast<uint8_t*>(data));
	}

private:
	using NodeKey = std::tuple<uint32_t, uint64_t, bool, bool>;

	[[nodiscard]] bool ValidCluster(uint64_t cluster) const {
		return cluster >= FirstClusterNumber && cluster - FirstClusterNumber < m_volume.cluster_count;
	}

	[[nodiscard]] uint64_t ClusterOffset(uint32_t cluster) const {
		return m_volume.heap_byte + (uint64_t{cluster} - FirstClusterNumber) * m_volume.cluster_bytes;
	}

	uint64_t InternNode(uint32_t first_cluster, uint64_t size, bool no_fat_chain, bool is_file) {
		const NodeKey key {first_cluster, size, no_fat_chain, is_file};
		if (const auto it = m_node_ids.find(key); it != m_node_ids.end()) {
			return it->second;
		}
		const uint64_t id = m_nodes.size();
		m_nodes.push_back(Node {first_cluster, size, no_fat_chain, is_file});
		m_node_ids.emplace(key, id);
		return id;
	}

	// Cluster after `cluster` in its chain. 0 at end of chain or on an invalid link.
	uint32_t NextInChain(uint32_t cluster) {
		if (!ValidCluster(cluster)) {
			return 0;
		}
		std::array<uint8_t, 4> slot {};
		if (!ReadBytes(m_file, m_volume.fat_byte + uint64_t{cluster} * 4, slot.data(), slot.size())) {
			return 0;
		}
		const uint32_t next = LoadU32(slot.data());
		return (next < MinEndOfChain && ValidCluster(next)) ? next : 0;
	}

	uint64_t ChainLength(uint32_t first) {
		uint64_t count   = 0;
		uint32_t cluster = first;
		while (cluster != 0 && count <= m_volume.cluster_count) {
			++count;
			cluster = NextInChain(cluster);
		}
		return count;
	}

	// Cluster holding chain position `index` of a node. 0 when the chain ends first.
	uint32_t ClusterAt(uint64_t id, const Node& node, uint64_t index) {
		if (node.no_fat_chain) {
			const uint64_t cluster = uint64_t{node.first_cluster} + index;
			return ValidCluster(cluster) ? static_cast<uint32_t>(cluster) : 0;
		}
		Cursor cursor {0, node.first_cluster};
		if (const auto it = m_cursors.find(id); it != m_cursors.end() && it->second.index <= index) {
			cursor = it->second;
		}
		while (cursor.index < index) {
			cursor.cluster = NextInChain(cursor.cluster);
			if (cursor.cluster == 0) {
				return 0;
			}
			++cursor.index;
		}
		if (!ValidCluster(cursor.cluster)) {
			return 0;
		}
		if (m_cursors.size() >= CursorCacheLimit) {
			m_cursors.clear();
		}
		m_cursors[id] = cursor;
		return cursor.cluster;
	}

	uint64_t ReadRange(uint64_t id, const Node& node, uint64_t offset, uint64_t size, uint8_t* data) {
		if (data == nullptr || offset >= node.size) {
			return 0;
		}
		const uint64_t want = std::min(size, node.size - offset);
		if (node.no_fat_chain) {
			return ReadBytes(m_file, ClusterOffset(node.first_cluster) + offset, data, want) ? want : 0;
		}
		uint64_t done = 0;
		while (done < want) {
			const uint64_t position = offset + done;
			const uint64_t within   = position % m_volume.cluster_bytes;
			const uint32_t cluster  = ClusterAt(id, node, position / m_volume.cluster_bytes);
			if (cluster == 0) {
				break;
			}
			const uint64_t step = std::min(m_volume.cluster_bytes - within, want - done);
			if (!ReadBytes(m_file, ClusterOffset(cluster) + within, data + done, step)) {
				break;
			}
			done += step;
		}
		return done;
	}

	bool ValidStream(uint32_t first_cluster, uint64_t size, bool no_fat_chain) const {
		if (size > uint64_t{m_volume.cluster_count} * m_volume.cluster_bytes) {
			return false;
		}
		if (size == 0) {
			return true;
		}
		if (!ValidCluster(first_cluster)) {
			return false;
		}
		if (no_fat_chain) {
			const uint64_t clusters = (size + m_volume.cluster_bytes - 1) / m_volume.cluster_bytes;
			return uint64_t{first_cluster} - FirstClusterNumber + clusters <= m_volume.cluster_count;
		}
		return true;
	}

	// Parses the file entry set starting at `set`: File, Stream Extension, then name entries.
	std::optional<Child> ParseFileSet(const uint8_t* set, size_t secondary_count) {
		const uint8_t* stream = set + DirEntryBytes;
		if (stream[0] != EntryStream) {
			return {};
		}
		const size_t name_length = stream[3];
		if (name_length == 0 || name_length > MaxNameUnits) {
			return {};
		}
		const bool     is_file      = (LoadU16(set + 4) & AttributeDirectory) == 0;
		const bool     no_fat_chain = (stream[1] & StreamNoFatChain) != 0;
		const uint32_t first        = LoadU32(stream + 20);
		const uint64_t size         = LoadU64(stream + 24);
		if (!ValidStream(first, size, no_fat_chain)) {
			return {};
		}

		std::u16string units;
		for (size_t index = 2; index <= secondary_count && units.size() < name_length; ++index) {
			const uint8_t* name = set + index * DirEntryBytes;
			if (name[0] != EntryName) {
				return {};
			}
			for (size_t unit = 0; unit < NameUnitsPerEntry && units.size() < name_length; ++unit) {
				units.push_back(static_cast<char16_t>(LoadU16(name + 2 + 2 * unit)));
			}
		}
		if (units.size() != name_length) {
			return {};
		}
		const uint64_t id = InternNode(first, size, no_fat_chain, is_file);
		return Child {Utf16ToUtf8(units), is_file, id};
	}

	// Entries after the first 0x00 are unused. Deleted or unknown entries are skipped one by one.
	Directory ParseEntries(const std::vector<uint8_t>& bytes) {
		Directory dir;
		for (size_t offset = 0; offset + DirEntryBytes <= bytes.size(); offset += DirEntryBytes) {
			const uint8_t* entry = bytes.data() + offset;
			if (entry[0] == EntryEndOfDirectory) {
				break;
			}
			if ((entry[0] & EntryInUse) == 0 || entry[0] != EntryFile) {
				continue;
			}
			const size_t secondary = entry[1];
			const size_t set_bytes = (secondary + 1) * DirEntryBytes;
			if (secondary < 2 || offset + set_bytes > bytes.size()) {
				continue;
			}
			if (LoadU16(entry + 2) != EntrySetChecksum(entry, set_bytes)) {
				continue;
			}
			if (auto child = ParseFileSet(entry, secondary)) {
				dir.by_name.emplace(FoldName(child->name), dir.children.size());
				dir.children.push_back(std::move(*child));
			}
			offset += set_bytes - DirEntryBytes;
		}
		return dir;
	}

	const Directory* LoadDirectory(uint64_t id) {
		if (const auto it = m_directories.find(id); it != m_directories.end()) {
			return &it->second;
		}
		const Node node = m_nodes[id];
		if (node.is_file || node.size > MaxDirectoryBytes) {
			return nullptr;
		}
		std::vector<uint8_t> bytes(node.size);
		if (ReadRange(id, node, 0, node.size, bytes.data()) != node.size) {
			return nullptr;
		}
		return &m_directories.emplace(id, ParseEntries(bytes)).first->second;
	}

	std::optional<Entry> FindChild(uint64_t dir_id, std::string_view name) {
		const Directory* dir = LoadDirectory(dir_id);
		if (dir == nullptr) {
			return {};
		}
		const auto it = dir->by_name.find(FoldName(name));
		if (it == dir->by_name.end()) {
			return {};
		}
		const Child& child = dir->children[it->second];
		return Entry {child.id, m_nodes[child.id].size, child.is_file};
	}

	std::optional<Entry> Resolve(std::string_view member) {
		Entry current {m_root, m_nodes[m_root].size, false};
		for (const auto part : SplitPath(member)) {
			if (current.is_file) {
				return {};
			}
			const auto child = FindChild(current.id, part);
			if (!child) {
				return {};
			}
			current = *child;
		}
		return current;
	}

	std::mutex                                m_mutex;
	std::ifstream                             m_file;
	Volume                                    m_volume;
	uint64_t                                  m_root = 0;
	std::vector<Node>                         m_nodes;
	std::map<NodeKey, uint64_t>               m_node_ids;
	std::unordered_map<uint64_t, Directory>   m_directories;
	std::unordered_map<uint64_t, Cursor>      m_cursors;
};

} // namespace

bool IsExfatImageFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	const auto file_size = FileSize(file);
	return file_size && FindVolume(file, *file_size).has_value();
}

std::shared_ptr<ArchiveReader> OpenExfatImage(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}
	const auto file_size = FileSize(file);
	if (!file_size) {
		return {};
	}
	const auto volume = FindVolume(file, *file_size);
	if (!volume) {
		return {};
	}
	auto reader = std::make_shared<ExfatReader>(std::move(file), *volume);
	if (!reader->Init()) {
		return {};
	}
	return reader;
}

} // namespace Common
