#ifndef KYTY_COMMON_PROCESS_MEMORY_H_
#define KYTY_COMMON_PROCESS_MEMORY_H_

#include <charconv>
#include <cstdint>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Common {

// Unknown is distinct from a genuine zero reading. Values are bytes, not KiB.
struct ProcessMemorySnapshot {
	std::optional<uint64_t> available;
	std::optional<uint64_t> resident;
	std::optional<uint64_t> anonymous;
	std::optional<uint64_t> swap;
};

inline std::optional<uint64_t> ParseProcMemoryKiB(std::string_view line, std::string_view key) {
	if (!line.starts_with(key)) return std::nullopt;
	line.remove_prefix(key.size());
	const auto start = line.find_first_not_of(" \t");
	if (start == std::string_view::npos) return std::nullopt;
	line.remove_prefix(start);
	uint64_t kib = 0;
	const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), kib);
	if (error != std::errc {} || kib > std::numeric_limits<uint64_t>::max() / 1024)
		return std::nullopt;
	std::string_view unit(end, line.data() + line.size() - end);
	const auto unit_start = unit.find_first_not_of(" \t");
	if (unit_start == std::string_view::npos) return std::nullopt;
	unit.remove_prefix(unit_start);
	const auto unit_end = unit.find_last_not_of(" \t\r");
	if (unit.substr(0, unit_end + 1) != "kB") return std::nullopt;
	return kib * 1024;
}

inline ProcessMemorySnapshot ParseProcessMemory(std::istream& meminfo, std::istream& status) {
	ProcessMemorySnapshot result;
	std::string line;
	while (std::getline(meminfo, line)) {
		if (const auto value = ParseProcMemoryKiB(line, "MemAvailable:")) result.available = value;
	}
	while (std::getline(status, line)) {
		if (const auto value = ParseProcMemoryKiB(line, "VmRSS:")) result.resident = value;
		if (const auto value = ParseProcMemoryKiB(line, "RssAnon:")) result.anonymous = value;
		if (const auto value = ParseProcMemoryKiB(line, "VmSwap:")) result.swap = value;
	}
	return result;
}

inline ProcessMemorySnapshot ReadProcessMemory() {
	std::ifstream meminfo("/proc/meminfo");
	std::ifstream status("/proc/self/status");
	return ParseProcessMemory(meminfo, status);
}

} // namespace Common

#endif
