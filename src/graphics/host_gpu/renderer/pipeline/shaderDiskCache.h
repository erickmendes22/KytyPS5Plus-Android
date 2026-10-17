#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDISKCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDISKCACHE_H_

#include "graphics/shader/shader.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Libs::Graphics {

// --shader-disk-cache: the guest programs a title translated, persisted per title so the next run
// translates them on background shader workers from boot instead of when the game first draws
// with them. Translation (the IR stage, 99% of shader cost) produces a resource plan that holds
// live IR, so the translated result itself is not persisted: the cache stores the translation
// inputs (guest code, user data, static input state) and the warm-up replays them. SPIR-V emission
// and module creation (~1% of the cost) run at first use as before, and the driver's pipeline
// binaries persist separately in _PipelineCache.
//
// Format (one file per title): an 8-byte magic and a signature (format version, the emulator
// binary's build id, the input-state layouts and the translation options), then append-only
// records, each with its own size and XXH3 checksum. A signature mismatch or an oversized file
// starts the cache over; a bad record truncates the file at the last good one.
class ShaderDiskCache {
public:
	using InputInfo = std::variant<ShaderVertexInputInfo, ShaderPixelInputInfo, ShaderComputeInputInfo>;

	struct Record {
		ShaderType               stage           = ShaderType::Unknown;
		uint64_t                 hash            = 0;
		uint32_t                 user_data_count = 0;
		std::array<uint32_t, 40> user_data {};
		std::vector<uint32_t>    code;
		std::vector<uint32_t>    back_code;
		InputInfo                input;
	};

	static constexpr uint64_t MaxFileBytes = 512ull << 20u;

	// Opens (creating or resetting as needed) the cache file and reads its valid records. Null when
	// the directory or file cannot be written; the reason is logged.
	static std::unique_ptr<ShaderDiskCache> Open(const std::filesystem::path& path,
	                                             uint64_t                     options_signature);
	~ShaderDiskCache();
	ShaderDiskCache(const ShaderDiskCache&)            = delete;
	ShaderDiskCache& operator=(const ShaderDiskCache&) = delete;

	[[nodiscard]] std::vector<Record> TakeLoaded() { return std::move(m_loaded); }
	// Appends and flushes one record (Android ends apps without running destructors). Stops
	// appending once the file reaches MaxFileBytes; the next start then begins a new cache.
	void Append(const Record& record);

	// Exposed for tests.
	[[nodiscard]] static std::vector<uint8_t>  Serialize(const Record& record);
	[[nodiscard]] static std::optional<Record> Deserialize(std::span<const uint8_t> payload);
	[[nodiscard]] static uint64_t              Signature(uint64_t options_signature);

private:
	ShaderDiskCache(std::FILE* file, std::filesystem::path path, uint64_t size);

	std::FILE*            m_file = nullptr;
	std::filesystem::path m_path;
	uint64_t              m_size = 0;
	std::vector<Record>   m_loaded;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERDISKCACHE_H_
