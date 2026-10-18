#pragma once

// Builds small exFAT volumes in memory for ExfatImageTests. Standard library only, so the
// host test compiles without the generated cmake_config.h.
//
// Layout: 512-byte sectors, 4 KiB clusters, one FAT, cluster heap right after the FAT.
// Files are contiguous (NoFatChain) unless `fragmented` is set; then every cluster is followed
// by a free gap and the FAT links the chain. Directories always use a FAT chain.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ExfatFixture {

struct Entry {
	std::string          path;                // '/'-separated; parents must be listed too
	std::vector<uint8_t> data;                // file contents, ignored for directories
	bool                 directory    = false;
	bool                 fragmented   = false; // FAT chain with gaps between clusters
	bool                 bad_checksum = false; // corrupt the entry set so readers skip it
};

inline constexpr uint32_t SectorBytes = 512;
inline constexpr uint32_t SectorsPerClusterShift = 3;
inline constexpr uint32_t ClusterBytes = SectorBytes << SectorsPerClusterShift;
inline constexpr uint64_t FirstPartitionLba = 2048;

inline void PutU16(uint8_t* p, uint16_t v) {
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

inline void PutU32(uint8_t* p, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

inline void PutU64(uint8_t* p, uint64_t v) {
	for (int i = 0; i < 8; ++i) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

// Decodes UTF-8 (valid input only) into UTF-16 code units.
inline std::u16string ToUtf16(std::string_view text) {
	std::u16string out;
	for (size_t i = 0; i < text.size();) {
		const auto lead = static_cast<unsigned char>(text[i]);
		uint32_t   cp   = lead;
		size_t     n    = 1;
		if (lead >= 0xF0) {
			cp = lead & 0x07u;
			n  = 4;
		} else if (lead >= 0xE0) {
			cp = lead & 0x0Fu;
			n  = 3;
		} else if (lead >= 0xC0) {
			cp = lead & 0x1Fu;
			n  = 2;
		}
		for (size_t k = 1; k < n; ++k) {
			cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
		}
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
			out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
		} else {
			out.push_back(static_cast<char16_t>(cp));
		}
		i += n;
	}
	return out;
}

inline uint16_t SetChecksum(const uint8_t* data, size_t size) {
	uint16_t sum = 0;
	for (size_t i = 0; i < size; ++i) {
		if (i == 2 || i == 3) {
			continue;
		}
		const auto rotated = static_cast<uint16_t>((sum << 15) | (sum >> 1));
		sum                = static_cast<uint16_t>(rotated + data[i]);
	}
	return sum;
}

class VolumeBuilder {
public:
	explicit VolumeBuilder(const std::vector<Entry>& entries): m_entries(entries) {}

	std::vector<uint8_t> Build() {
		auto root_bytes = DirectoryBytes("");
		const auto root = Allocate(root_bytes.size(), true, true);
		Write(root, root_bytes);
		return Assemble(root.front());
	}

private:
	struct Placed {
		uint32_t first_cluster = 0;
		uint64_t size          = 0;
		bool     no_fat_chain  = false;
		bool     is_dir        = false;
	};

	static std::string ParentOf(const std::string& path) {
		const auto slash = path.rfind('/');
		return slash == std::string::npos ? std::string() : path.substr(0, slash);
	}

	static std::string BaseOf(const std::string& path) {
		const auto slash = path.rfind('/');
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	std::vector<const Entry*> ChildrenOf(const std::string& dir) const {
		std::vector<const Entry*> children;
		for (const auto& entry : m_entries) {
			if (ParentOf(entry.path) == dir) {
				children.push_back(&entry);
			}
		}
		return children;
	}

	// Serializes a directory's entry sets, placing every child first. Padded to whole clusters.
	std::vector<uint8_t> DirectoryBytes(const std::string& dir) {
		std::vector<uint8_t> bytes;
		for (const auto* child : ChildrenOf(dir)) {
			const auto placed = Place(*child);
			AppendEntrySet(bytes, *child, placed);
		}
		bytes.resize((bytes.size() + ClusterBytes - 1) / ClusterBytes * ClusterBytes, 0);
		if (bytes.empty()) {
			bytes.resize(ClusterBytes, 0);
		}
		return bytes;
	}

	Placed Place(const Entry& entry) {
		Placed placed;
		if (entry.directory) {
			auto bytes           = DirectoryBytes(entry.path);
			const auto clusters  = Allocate(bytes.size(), true, entry.fragmented);
			Write(clusters, bytes);
			placed.first_cluster = clusters.front();
			placed.size          = bytes.size();
			placed.is_dir        = true;
			return placed;
		}
		const auto clusters = Allocate(entry.data.size(), entry.fragmented, entry.fragmented);
		Write(clusters, entry.data);
		placed.first_cluster = clusters.empty() ? 0 : clusters.front();
		placed.size          = entry.data.size();
		placed.no_fat_chain  = !entry.fragmented;
		return placed;
	}

	void AppendEntrySet(std::vector<uint8_t>& out, const Entry& entry, const Placed& placed) {
		const auto name = ToUtf16(BaseOf(entry.path));
		const auto name_entries = (name.size() + 14) / 15;
		const auto secondary    = 1 + name_entries;
		const auto set_bytes    = (secondary + 1) * 32;
		const auto start        = out.size();
		out.resize(start + set_bytes, 0);
		uint8_t* p = out.data() + start;

		p[0] = 0x85;
		p[1] = static_cast<uint8_t>(secondary);
		PutU16(p + 4, placed.is_dir ? 0x10 : 0x20);

		uint8_t* stream = p + 32;
		stream[0]       = 0xC0;
		stream[1]       = static_cast<uint8_t>(placed.no_fat_chain ? 0x03 : 0x01);
		stream[3]       = static_cast<uint8_t>(name.size());
		PutU64(stream + 8, placed.size);
		PutU32(stream + 20, placed.first_cluster);
		PutU64(stream + 24, placed.size);

		for (size_t i = 0; i < name_entries; ++i) {
			uint8_t* n = p + (2 + i) * 32;
			n[0]       = 0xC1;
			for (size_t k = 0; k < 15; ++k) {
				const auto index = i * 15 + k;
				if (index < name.size()) {
					PutU16(n + 2 + 2 * k, static_cast<uint16_t>(name[index]));
				}
			}
		}

		PutU16(p + 2, SetChecksum(p, set_bytes));
		if (entry.bad_checksum) {
			p[2] = static_cast<uint8_t>(p[2] ^ 0xFF);
		}
	}

	// Returns the clusters for `bytes` of data. `gaps` leaves a free cluster after each one, so the
	// chain is non-contiguous; `chain` writes FAT links instead of setting NoFatChain.
	std::vector<uint32_t> Allocate(size_t bytes, bool chain, bool gaps) {
		std::vector<uint32_t> clusters;
		const auto            count  = (bytes + ClusterBytes - 1) / ClusterBytes;
		const auto            stride = gaps ? 2u : 1u;
		for (size_t i = 0; i < count; ++i) {
			clusters.push_back(m_next);
			m_max  = m_next > m_max ? m_next : m_max;
			m_next += stride;
		}
		if (m_fat.size() < m_max + 2) {
			m_fat.resize(m_max + 2, 0);
		}
		if (chain) {
			for (size_t i = 0; i + 1 < clusters.size(); ++i) {
				m_fat[clusters[i]] = clusters[i + 1];
			}
			if (!clusters.empty()) {
				m_fat[clusters.back()] = 0xFFFFFFFFu;
			}
		}
		return clusters;
	}

	void Write(const std::vector<uint32_t>& clusters, const std::vector<uint8_t>& bytes) {
		size_t offset = 0;
		for (const auto cluster : clusters) {
			const auto pos = (static_cast<size_t>(cluster) - 2) * ClusterBytes;
			if (m_heap.size() < pos + ClusterBytes) {
				m_heap.resize(pos + ClusterBytes, 0);
			}
			const auto n = std::min<size_t>(ClusterBytes, bytes.size() - offset);
			for (size_t i = 0; i < n; ++i) {
				m_heap[pos + i] = bytes[offset + i];
			}
			offset += n;
		}
	}

	std::vector<uint8_t> Assemble(uint32_t root_cluster) {
		const auto cluster_count = static_cast<uint32_t>(m_max - 1);
		m_fat.resize(cluster_count + 2, 0);
		m_fat[0] = 0xFFFFFFF8u;
		m_fat[1] = 0xFFFFFFFFu;

		const uint64_t fat_bytes   = static_cast<uint64_t>(cluster_count + 2) * 4;
		const uint64_t fat_sectors = (fat_bytes + SectorBytes - 1) / SectorBytes;
		const uint64_t fat_offset  = 24;
		const uint64_t heap_offset = fat_offset + fat_sectors;
		const uint64_t volume_sectors = heap_offset + (static_cast<uint64_t>(cluster_count) << SectorsPerClusterShift);

		std::vector<uint8_t> image(volume_sectors * SectorBytes, 0);
		uint8_t*             boot = image.data();
		boot[0]                   = 0xEB;
		boot[1]                   = 0x76;
		boot[2]                   = 0x90;
		for (size_t i = 0; i < 8; ++i) {
			boot[3 + i] = static_cast<uint8_t>("EXFAT   "[i]);
		}
		PutU64(boot + 72, volume_sectors);
		PutU32(boot + 80, static_cast<uint32_t>(fat_offset));
		PutU32(boot + 84, static_cast<uint32_t>(fat_sectors));
		PutU32(boot + 88, static_cast<uint32_t>(heap_offset));
		PutU32(boot + 92, cluster_count);
		PutU32(boot + 96, root_cluster);
		PutU32(boot + 100, 0x12345678u);
		PutU16(boot + 104, 0x0100);
		boot[108] = 9;
		boot[109] = SectorsPerClusterShift;
		boot[110] = 1;
		boot[111] = 0x80;
		boot[112] = 0;
		boot[510] = 0x55;
		boot[511] = 0xAA;

		uint8_t* fat = image.data() + fat_offset * SectorBytes;
		for (size_t i = 0; i < m_fat.size(); ++i) {
			PutU32(fat + 4 * i, m_fat[i]);
		}
		std::copy(m_heap.begin(), m_heap.end(), image.begin() + heap_offset * SectorBytes);
		return image;
	}

	const std::vector<Entry>& m_entries;
	std::vector<uint8_t>      m_heap;
	std::vector<uint32_t>     m_fat;
	uint32_t                  m_next = 2;
	uint32_t                  m_max  = 1;
};

// A raw exFAT volume: the boot sector is at byte 0.
inline std::vector<uint8_t> BuildExfatVolume(const std::vector<Entry>& entries) {
	return VolumeBuilder(entries).Build();
}

// Wraps a volume as the single exFAT partition of an MBR disk (type 0x07 at LBA 2048).
inline std::vector<uint8_t> WrapInMbr(const std::vector<uint8_t>& volume) {
	std::vector<uint8_t> disk(FirstPartitionLba * SectorBytes + volume.size(), 0);
	uint8_t*             mbr = disk.data();
	mbr[446 + 4]           = 0x07;
	PutU32(mbr + 446 + 8, static_cast<uint32_t>(FirstPartitionLba));
	PutU32(mbr + 446 + 12, static_cast<uint32_t>(volume.size() / SectorBytes));
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	std::copy(volume.begin(), volume.end(), disk.begin() + FirstPartitionLba * SectorBytes);
	return disk;
}

// Wraps a volume in a GPT disk: protective MBR, primary header at LBA 1, one entry at LBA 2.
inline std::vector<uint8_t> WrapInGpt(const std::vector<uint8_t>& volume) {
	const uint64_t volume_sectors = volume.size() / SectorBytes;
	std::vector<uint8_t> disk(FirstPartitionLba * SectorBytes + volume.size(), 0);
	uint8_t*             mbr = disk.data();
	mbr[446 + 4]           = 0xEE;
	PutU32(mbr + 446 + 8, 1);
	PutU32(mbr + 446 + 12, 0xFFFFFFFFu);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	uint8_t* header = disk.data() + SectorBytes;
	for (size_t i = 0; i < 8; ++i) {
		header[i] = static_cast<uint8_t>("EFI PART"[i]);
	}
	PutU32(header + 8, 0x00010000u);
	PutU32(header + 12, 92);
	PutU64(header + 24, 1);
	PutU64(header + 40, FirstPartitionLba);
	PutU64(header + 72, 2);
	PutU32(header + 80, 128);
	PutU32(header + 84, 128);

	uint8_t* entry = disk.data() + 2 * SectorBytes;
	entry[0]       = 0xA2;
	entry[1]       = 0xD0;
	PutU64(entry + 32, FirstPartitionLba);
	PutU64(entry + 40, FirstPartitionLba + volume_sectors - 1);
	std::copy(volume.begin(), volume.end(), disk.begin() + FirstPartitionLba * SectorBytes);
	return disk;
}

} // namespace ExfatFixture
