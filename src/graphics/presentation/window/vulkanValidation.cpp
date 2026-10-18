#include "graphics/presentation/window/vulkanValidation.h"

#include "common/logging/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Libs::Graphics::VulkanValidation {

namespace {

constexpr const char* kLayerName = "VK_LAYER_KHRONOS_validation";
// Distinct messages kept with their own count; later new ones only add to the overflow count.
constexpr size_t kMaxEntries = 4096;
// A message's full text is logged on its first occurrence, a short line at counts 10, 100, ...
constexpr uint64_t kSummaryIntervalSeconds = 30;
constexpr size_t   kSummaryRows            = 60;
constexpr size_t   kLogcatChunk            = 900;

Options Parse(const char* value) {
	Options options;
	if (value == nullptr || *value == '\0' || std::string_view(value) == "0") {
		return options;
	}
	options.requested = true;
	if (std::string_view(value) == "1") {
		// GPU-AV is opt-in: on Turnip v32 it hangs Astro Bot's boot (all threads idle in futex
		// waits at ~4 s guest time, nothing presented), while Vampire Survivors boots with it.
		options.core = true;
		options.sync = true;
		return options;
	}
	std::string_view rest(value);
	while (!rest.empty()) {
		// '+' because --env values travel inside the comma-separated launch arguments.
		const auto comma = rest.find_first_of(",+");
		const auto token = rest.substr(0, comma);
		if (token == "core") {
			options.core = true;
		} else if (token == "sync") {
			options.sync = true;
		} else if (token == "gpuav") {
			options.gpuav = true;
		} else if (token == "limit") {
			options.limit = true;
		} else {
			LOGF("[vvl] KYTY_VK_VALIDATION: unknown option '%.*s' ignored\n",
			     static_cast<int>(token.size()), token.data());
		}
		rest = comma == std::string_view::npos ? std::string_view {} : rest.substr(comma + 1);
	}
	return options;
}

struct Entry {
	std::string severity;
	std::string id_name;
	std::string first_message;
	uint64_t    count      = 0;
	uint64_t    first_secs = 0;
};

struct State {
	std::mutex                             mutex;
	std::unordered_map<std::string, Entry> entries;
	uint64_t                               total    = 0;
	uint64_t                               overflow = 0;
	std::chrono::steady_clock::time_point  start    = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point  last_summary = start;
};

State& GetState() {
	static State state;
	return state;
}

uint64_t SecondsSince(std::chrono::steady_clock::time_point start) {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
	        .count());
}

bool IsWordChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

void Emit(const std::string& line) {
	LOGF("%s\n", line.c_str());
#if defined(__ANDROID__)
	// Logcat truncates long lines; the emulator log keeps the whole text.
	for (size_t offset = 0; offset < line.size(); offset += kLogcatChunk) {
		__android_log_write(ANDROID_LOG_WARN, "KytyVVL", line.substr(offset, kLogcatChunk).c_str());
	}
#endif
}

bool IsMilestone(uint64_t count) {
	if (count < 10) {
		return false;
	}
	while (count % 10 == 0) {
		count /= 10;
	}
	return count == 1;
}

std::string SummaryLocked(State& state, const char* why) {
	std::vector<const std::pair<const std::string, Entry>*> rows;
	rows.reserve(state.entries.size());
	for (const auto& entry: state.entries) {
		rows.push_back(&entry);
	}
	std::sort(rows.begin(), rows.end(),
	          [](const auto* a, const auto* b) { return a->second.count > b->second.count; });
	std::string text = fmt::format("[vvl] summary ({}) at +{} s: {} messages, {} distinct, {} "
	                               "uncounted (table full)\n",
	                               why, SecondsSince(state.start), state.total,
	                               state.entries.size(), state.overflow);
	for (size_t i = 0; i < rows.size() && i < kSummaryRows; i++) {
		const auto& entry = rows[i]->second;
		text += fmt::format("[vvl]   {:>9} x {} {} (first +{} s): {:.300}\n", entry.count,
		                    entry.severity, entry.id_name, entry.first_secs, entry.first_message);
	}
	return text;
}

} // namespace

const Options& Requested() {
	static const Options options = Parse(std::getenv("KYTY_VK_VALIDATION"));
	return options;
}

LayerSettings::LayerSettings() {
	const auto& options = Requested();
	AddBool("validate_core", options.core);
	AddBool("validate_sync", options.sync);
	AddBool("syncval_submit_time_validation", options.sync);
	// Shader accesses from SPIR-V analysis instead of "every bound descriptor is accessed".
	AddBool("syncval_shader_accesses_heuristic", options.sync);
	AddBool("syncval_message_extra_properties", options.sync);
	AddBool("gpuav_enable", options.gpuav);
	AddBool("gpuav_shader_instrumentation", options.gpuav);
	AddBool("gpuav_descriptor_checks", options.gpuav);
	AddBool("gpuav_buffer_address_oob", options.gpuav);
	AddBool("gpuav_safe_mode", false);
	AddBool("validate_best_practices", false);
	AddBool("printf_enable", false);
	AddBool("enable_message_limit", options.limit);

	// m_bools is complete: the settings can point into it now.
	for (size_t i = 0; i < m_settings.size(); i++) {
		m_settings[i].pValues = &m_bools[i];
	}
	m_info.settingCount = static_cast<uint32_t>(m_settings.size());
	m_info.pSettings    = m_settings.data();

	for (size_t i = 0; i < m_settings.size(); i++) {
		LOGF("[vvl] layer setting %s = %u\n", m_settings[i].pSettingName, m_bools[i]);
	}
}

void LayerSettings::AddBool(const char* name, bool value) {
	m_bools.push_back(value ? VK_TRUE : VK_FALSE);
	vk::LayerSettingEXT setting {};
	setting.pLayerName   = kLayerName;
	setting.pSettingName = name;
	setting.type         = vk::LayerSettingTypeEXT::eBool32;
	setting.valueCount   = 1;
	m_settings.push_back(setting);
}

std::string DedupKey(const char* id_name, const char* message) {
	std::string key = id_name != nullptr ? id_name : "?";
	key += '|';
	const std::string_view text = message != nullptr ? message : "";
	for (size_t i = 0; i < text.size();) {
		const bool word_start = i == 0 || !IsWordChar(text[i - 1]);
		if (word_start && text.substr(i).starts_with("0x")) {
			// Handles and addresses: 0x followed by hex digits.
			size_t end = i + 2;
			while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])) != 0) {
				end++;
			}
			key += "0x?";
			i = end;
		} else if (word_start && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
			// Free-standing decimal numbers (sequence numbers, offsets, sizes); digits inside a
			// word such as a shader hash label (cs_48bc4ef0...) are kept.
			size_t end = i;
			while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
				end++;
			}
			if (end < text.size() && IsWordChar(text[end])) {
				key.append(text.substr(i, end - i));
			} else {
				key += '#';
			}
			i = end;
		} else {
			key += text[i++];
		}
	}
	return key;
}

VKAPI_ATTR vk::Bool32 VKAPI_CALL MessengerCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT      severity,
    vk::DebugUtilsMessageTypeFlagsEXT             /*types*/,
    const vk::DebugUtilsMessengerCallbackDataEXT* data, void* /*user_data*/) {
	if (data == nullptr || data->pMessage == nullptr ||
	    severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose ||
	    severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
		return VK_FALSE;
	}
	const char* severity_name =
	    severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError ? "E" : "W";
	const char* id_name = data->pMessageIdName != nullptr ? data->pMessageIdName : "?";
	// Core VUIDs do not name the debug region; the open command-buffer labels do.
	std::string text = data->pMessage;
	if (data->cmdBufLabelCount > 0 && data->pCmdBufLabels != nullptr) {
		text += " [cmd labels:";
		for (uint32_t i = 0; i < data->cmdBufLabelCount; i++) {
			const char* name = data->pCmdBufLabels[i].pLabelName;
			text += ' ';
			text += name != nullptr ? name : "?";
		}
		text += ']';
	}
	auto key = DedupKey(id_name, text.c_str());

	auto&       state = GetState();
	std::string line;
	std::string summary;
	{
		const std::lock_guard lock(state.mutex);
		state.total++;
		auto it = state.entries.find(key);
		if (it == state.entries.end()) {
			if (state.entries.size() >= kMaxEntries) {
				state.overflow++;
				return VK_FALSE;
			}
			Entry entry {.severity      = severity_name,
			             .id_name       = id_name,
			             .first_message = text,
			             .count         = 0,
			             .first_secs    = SecondsSince(state.start)};
			it = state.entries.emplace(std::move(key), std::move(entry)).first;
		}
		auto& entry = it->second;
		entry.count++;
		if (entry.count == 1) {
			line = fmt::format("[vvl] NEW #{} {} {} +{} s: {}", state.entries.size(),
			                   severity_name, id_name, entry.first_secs, text);
		} else if (IsMilestone(entry.count)) {
			line = fmt::format("[vvl] x{} {} {}: {:.200}", entry.count, severity_name, id_name,
			                   entry.first_message);
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - state.last_summary >= std::chrono::seconds(kSummaryIntervalSeconds)) {
			state.last_summary = now;
			summary            = SummaryLocked(state, "periodic");
		}
	}
	if (!line.empty()) {
		Emit(line);
	}
	if (!summary.empty()) {
		LOGF("%s", summary.c_str());
	}
	return VK_FALSE;
}

void LogSummary(const char* why) {
	auto& state = GetState();
	// A fatal error may arrive while another thread is inside the callback; do not wait forever.
	std::unique_lock lock(state.mutex, std::defer_lock);
	for (int attempt = 0; attempt < 200 && !lock.try_lock(); attempt++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!lock.owns_lock()) {
		Emit("[vvl] summary skipped: message table busy");
		return;
	}
	const auto summary = SummaryLocked(state, why);
	lock.unlock();
	LOGF("%s", summary.c_str());
#if defined(__ANDROID__)
	size_t begin = 0;
	while (begin < summary.size()) {
		const auto end = summary.find('\n', begin);
		const auto row = summary.substr(begin, end == std::string::npos ? std::string::npos
		                                                                : end - begin);
		__android_log_write(ANDROID_LOG_WARN, "KytyVVL", row.substr(0, kLogcatChunk).c_str());
		begin = end == std::string::npos ? summary.size() : end + 1;
	}
#endif
}

} // namespace Libs::Graphics::VulkanValidation
