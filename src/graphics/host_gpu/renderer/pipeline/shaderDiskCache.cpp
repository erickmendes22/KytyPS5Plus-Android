#include "graphics/host_gpu/renderer/pipeline/shaderDiskCache.h"

#include "common/logging/log.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include <cstdlib>
#include <fmt/format.h>
#include <system_error>
#include <type_traits>
#include <xxhash.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <elf.h>
#include <link.h>
#endif

namespace Libs::Graphics {

namespace {

static_assert(std::is_trivially_copyable_v<ShaderVertexInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderPixelInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderComputeInputInfo>);

constexpr char     FileMagic[8]   = {'K', 'Y', 'S', 'H', 'D', 'C', 'C', '1'};
constexpr uint32_t RecordMagic    = 0x52534b59u; // "YKSR"
constexpr uint32_t FormatVersion  = 1;
constexpr uint32_t MaxCodeWords   = 1u << 20u;
constexpr size_t   HeaderBytes    = sizeof(FileMagic) + sizeof(uint64_t);
constexpr size_t   RecordHeadSize = sizeof(uint32_t) * 2 + sizeof(uint64_t);

void CacheLog(const std::string& line) {
	Log::WriteToConsoleAndLog("Shader disk cache: " + line + "\n");
}

#if defined(__linux__) || defined(__ANDROID__)
// The ELF NT_GNU_BUILD_ID of the module holding this code: changes with every different build of
// the emulator binary, including uncommitted ("-dirty") ones.
std::string ModuleBuildId() {
	struct Search {
		uintptr_t   address = 0;
		std::string id;
	} search {reinterpret_cast<uintptr_t>(&ModuleBuildId), {}};
	dl_iterate_phdr(
	    [](dl_phdr_info* info, size_t, void* data) -> int {
		    auto* s = static_cast<Search*>(data);
		    bool  contains = false;
		    for (int i = 0; i < info->dlpi_phnum && !contains; i++) {
			    const auto& ph    = info->dlpi_phdr[i];
			    const auto  start = info->dlpi_addr + ph.p_vaddr;
			    contains = ph.p_type == PT_LOAD && s->address >= start && s->address < start + ph.p_memsz;
		    }
		    if (!contains) return 0;
		    for (int i = 0; i < info->dlpi_phnum; i++) {
			    const auto& ph = info->dlpi_phdr[i];
			    if (ph.p_type != PT_NOTE) continue;
			    const auto* p   = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
			    const auto* end = p + ph.p_memsz;
			    while (p + sizeof(ElfW(Nhdr)) <= end) {
				    const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
				    const auto* name = p + sizeof(ElfW(Nhdr));
				    const auto* desc = name + ((note->n_namesz + 3u) & ~3u);
				    if (desc + note->n_descsz > end) break;
				    if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
				        std::memcmp(name, "GNU", 4) == 0) {
					    for (uint32_t b = 0; b < note->n_descsz; b++) s->id += fmt::format("{:02x}", desc[b]);
					    return 1;
				    }
				    p = desc + ((note->n_descsz + 3u) & ~3u);
			    }
		    }
		    return 1;
	    },
	    &search);
	return search.id;
}
#else
std::string ModuleBuildId() {
	return {};
}
#endif

size_t InputSize(ShaderType stage) {
	switch (stage) {
		case ShaderType::Pixel: return sizeof(ShaderPixelInputInfo);
		case ShaderType::Compute: return sizeof(ShaderComputeInputInfo);
		case ShaderType::Vertex:
		case ShaderType::Mesh:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation: return sizeof(ShaderVertexInputInfo);
		default: return 0;
	}
}

class Writer {
public:
	template <typename T>
	void Put(const T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		m_out.insert(m_out.end(), bytes, bytes + sizeof(T));
	}
	void PutWords(std::span<const uint32_t> words) {
		const auto* bytes = reinterpret_cast<const uint8_t*>(words.data());
		m_out.insert(m_out.end(), bytes, bytes + words.size_bytes());
	}
	std::vector<uint8_t> Take() { return std::move(m_out); }

private:
	std::vector<uint8_t> m_out;
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> in): m_in(in) {}
	template <typename T>
	bool Get(T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		if (m_in.size() - m_pos < sizeof(T)) return false;
		std::memcpy(&value, m_in.data() + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return true;
	}
	bool GetWords(std::vector<uint32_t>& words, uint32_t count) {
		const size_t bytes = static_cast<size_t>(count) * sizeof(uint32_t);
		if (m_in.size() - m_pos < bytes) return false;
		words.resize(count);
		std::memcpy(words.data(), m_in.data() + m_pos, bytes);
		m_pos += bytes;
		return true;
	}
	[[nodiscard]] bool AtEnd() const { return m_pos == m_in.size(); }

private:
	std::span<const uint8_t> m_in;
	size_t                   m_pos = 0;
};

template <typename T>
T WithoutRuntime(T input) {
	input.stage = {}; // pointers into this session's caches
	return input;
}

bool WriteHeader(std::FILE* file, uint64_t signature) {
	return std::fwrite(FileMagic, sizeof(FileMagic), 1, file) == 1 &&
	       std::fwrite(&signature, sizeof(signature), 1, file) == 1 && std::fflush(file) == 0;
}

// Reads the records after the header; returns the offset just past the last valid one.
uint64_t ReadRecords(std::FILE* file, uint64_t file_size, std::vector<ShaderDiskCache::Record>& out) {
	uint64_t             good = HeaderBytes;
	std::vector<uint8_t> payload;
	while (good + RecordHeadSize <= file_size) {
		uint32_t magic = 0;
		uint32_t size  = 0;
		uint64_t check = 0;
		if (std::fread(&magic, sizeof(magic), 1, file) != 1 || std::fread(&size, sizeof(size), 1, file) != 1 ||
		    std::fread(&check, sizeof(check), 1, file) != 1 || magic != RecordMagic ||
		    size > file_size - good - RecordHeadSize) {
			break;
		}
		payload.resize(size);
		if (size != 0 && std::fread(payload.data(), size, 1, file) != 1) break;
		if (XXH3_64bits(payload.data(), payload.size()) != check) break;
		auto record = ShaderDiskCache::Deserialize(payload);
		if (!record) break;
		out.push_back(std::move(*record));
		good += RecordHeadSize + size;
	}
	return good;
}

} // namespace

uint64_t ShaderDiskCache::Signature(uint64_t options_signature) {
	const auto text = fmt::format("v{} {} {} vs{} ps{} cs{} opt{:016x}", FormatVersion, ModuleBuildId(),
	                              KYTY_GIT_REVISION, sizeof(ShaderVertexInputInfo),
	                              sizeof(ShaderPixelInputInfo), sizeof(ShaderComputeInputInfo),
	                              options_signature);
	return XXH3_64bits(text.data(), text.size());
}

std::vector<uint8_t> ShaderDiskCache::Serialize(const Record& record) {
	Writer w;
	w.Put(static_cast<uint32_t>(record.stage));
	w.Put(record.hash);
	w.Put(record.user_data_count);
	w.Put(static_cast<uint32_t>(record.code.size()));
	w.Put(static_cast<uint32_t>(record.back_code.size()));
	w.Put(record.user_data);
	w.PutWords(record.code);
	w.PutWords(record.back_code);
	std::visit([&w](const auto& input) { w.Put(WithoutRuntime(input)); }, record.input);
	return w.Take();
}

std::optional<ShaderDiskCache::Record> ShaderDiskCache::Deserialize(std::span<const uint8_t> payload) {
	Reader   r(payload);
	Record   record;
	uint32_t stage      = 0;
	uint32_t code_words = 0;
	uint32_t back_words = 0;
	if (!r.Get(stage) || !r.Get(record.hash) || !r.Get(record.user_data_count) || !r.Get(code_words) ||
	    !r.Get(back_words) || !r.Get(record.user_data)) {
		return std::nullopt;
	}
	record.stage = static_cast<ShaderType>(stage);
	if (InputSize(record.stage) == 0 || code_words == 0 || code_words > MaxCodeWords ||
	    back_words > MaxCodeWords || record.user_data_count > record.user_data.size() ||
	    !r.GetWords(record.code, code_words) || !r.GetWords(record.back_code, back_words)) {
		return std::nullopt;
	}
	bool ok = false;
	switch (record.stage) {
		case ShaderType::Pixel: {
			ShaderPixelInputInfo input {};
			ok           = r.Get(input);
			record.input = input;
			break;
		}
		case ShaderType::Compute: {
			ShaderComputeInputInfo input {};
			ok           = r.Get(input);
			record.input = input;
			break;
		}
		default: {
			ShaderVertexInputInfo input {};
			ok           = r.Get(input) && input.logical_stage == record.stage;
			record.input = input;
			break;
		}
	}
	if (!ok || !r.AtEnd()) {
		return std::nullopt;
	}
	return record;
}

namespace {

// KYTY_SHADER_CACHE_PER_BUILD (default on): one cache file per signature (build + shader options),
// <title>-<signature>.bin, so switching between builds or option sets (A/B benchmarks, several
// test builds sharing a device) reloads that build's file instead of retranslating every shader
// at startup. The files of the 3 most recently used other signatures are kept.
bool PerBuildFiles() {
	const char* value = std::getenv("KYTY_SHADER_CACHE_PER_BUILD");
	return value == nullptr || value[0] != '0';
}

std::filesystem::path PerBuildPath(const std::filesystem::path& path, uint64_t signature) {
	return path.parent_path() /
	       fmt::format("{}-{:016x}{}", path.stem().string(), signature, path.extension().string());
}

void PruneOtherBuilds(const std::filesystem::path& path, const std::filesystem::path& current) {
	constexpr size_t kKeepOthers = 3;
	std::error_code  error;
	const auto       prefix = path.stem().string() + "-";
	std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> others;
	for (const auto& entry: std::filesystem::directory_iterator(path.parent_path(), error)) {
		const auto name = entry.path().filename().string();
		if (entry.path() != current && name.rfind(prefix, 0) == 0 &&
		    entry.path().extension() == path.extension()) {
			std::error_code time_error;
			others.emplace_back(entry.last_write_time(time_error), entry.path());
		}
	}
	if (others.size() <= kKeepOthers) {
		return;
	}
	std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	for (size_t i = kKeepOthers; i < others.size(); i++) {
		std::error_code remove_error;
		std::filesystem::remove(others[i].second, remove_error);
		CacheLog(fmt::format("removed {} (older build)", others[i].second.string()));
	}
}

} // namespace

std::unique_ptr<ShaderDiskCache> ShaderDiskCache::Open(const std::filesystem::path& base_path,
                                                       uint64_t                     options_signature) {
	std::error_code error;
	std::filesystem::create_directories(base_path.parent_path(), error);
	const auto signature = Signature(options_signature);
	const auto path      = PerBuildFiles() ? PerBuildPath(base_path, signature) : base_path;
	if (PerBuildFiles()) {
		PruneOtherBuilds(base_path, path);
	}
	const auto name = path.string();

	std::vector<Record> loaded;
	uint64_t            keep   = 0;
	const auto          exists = std::filesystem::is_regular_file(path, error);
	const auto          size   = exists ? std::filesystem::file_size(path, error) : 0;
	if (exists && !error) {
		if (size > MaxFileBytes) {
			CacheLog(fmt::format("{} is {} MiB, over the {} MiB cap; starting over", name, size >> 20u,
			                     MaxFileBytes >> 20u));
		} else if (std::FILE* in = std::fopen(name.c_str(), "rb"); in != nullptr) {
			char     magic[sizeof(FileMagic)] = {};
			uint64_t stored                   = 0;
			if (std::fread(magic, sizeof(magic), 1, in) == 1 && std::fread(&stored, sizeof(stored), 1, in) == 1 &&
			    std::memcmp(magic, FileMagic, sizeof(magic)) == 0 && stored == signature) {
				keep = ReadRecords(in, size, loaded);
				if (keep != size) {
					CacheLog(fmt::format("{}: discarding {} bytes after the last valid record", name,
					                     size - keep));
				}
			} else {
				CacheLog(fmt::format("{} was written by another build or with other options; starting over",
				                     name));
			}
			std::fclose(in);
		}
	}

	std::FILE* file = nullptr;
	if (keep >= HeaderBytes) {
		std::filesystem::resize_file(path, keep, error);
		file = error ? nullptr : std::fopen(name.c_str(), "ab");
	} else {
		loaded.clear();
		file = std::fopen(name.c_str(), "wb");
		if (file != nullptr && !WriteHeader(file, signature)) {
			std::fclose(file);
			file = nullptr;
		}
		keep = HeaderBytes;
	}
	if (file == nullptr) {
		CacheLog(fmt::format("cannot write {}; disabled", name));
		return nullptr;
	}
	CacheLog(fmt::format("{} programs ({} KiB) from {}", loaded.size(), keep >> 10u, name));
	auto cache      = std::unique_ptr<ShaderDiskCache>(new ShaderDiskCache(file, path, keep));
	cache->m_loaded = std::move(loaded);
	return cache;
}

ShaderDiskCache::ShaderDiskCache(std::FILE* file, std::filesystem::path path, uint64_t size)
    : m_file(file), m_path(std::move(path)), m_size(size) {}

ShaderDiskCache::~ShaderDiskCache() {
	if (m_file != nullptr) {
		std::fclose(m_file);
	}
}

void ShaderDiskCache::Append(const Record& record) {
	if (m_file == nullptr) {
		return;
	}
	const auto payload = Serialize(record);
	if (m_size + RecordHeadSize + payload.size() > MaxFileBytes) {
		CacheLog(fmt::format("{} reached the {} MiB cap; not adding more", m_path.string(),
		                     MaxFileBytes >> 20u));
		std::fclose(m_file);
		m_file = nullptr;
		return;
	}
	const auto size  = static_cast<uint32_t>(payload.size());
	const auto check = XXH3_64bits(payload.data(), payload.size());
	const bool ok    = std::fwrite(&RecordMagic, sizeof(RecordMagic), 1, m_file) == 1 &&
	                std::fwrite(&size, sizeof(size), 1, m_file) == 1 &&
	                std::fwrite(&check, sizeof(check), 1, m_file) == 1 &&
	                std::fwrite(payload.data(), payload.size(), 1, m_file) == 1 && std::fflush(m_file) == 0;
	if (!ok) {
		// A torn record is dropped by the checksum at the next start.
		CacheLog(fmt::format("write to {} failed; disabled", m_path.string()));
		std::fclose(m_file);
		m_file = nullptr;
		return;
	}
	m_size += RecordHeadSize + payload.size();
}

} // namespace Libs::Graphics
