#include "graphics/host_gpu/renderer/liveKnobs.h"

#include "common/logging/log.h"
#include "common/perfWaits.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineStats.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <fmt/format.h>
#include <sys/stat.h>

namespace Libs::Graphics::LiveKnobs {

namespace {

constexpr const char* kFileName     = "_live.txt";
constexpr int64_t     kPollInterval = 1'000'000'000;
constexpr size_t      kMaxFileBytes = 4096;
constexpr int         kMaxShakeSeconds = 60;

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LIVE_KNOBS");
		const bool  on    = value != nullptr && std::strcmp(value, "1") == 0;
		if (on) {
			LOGF("Live knobs: polling %s once a second\n", kFileName);
		}
		return on;
	}();
	return enabled;
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

std::string_view Trim(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
		text.remove_suffix(1);
	}
	return text;
}

std::vector<std::string> ParsePrefixes(std::string_view value) {
	auto prefixes = PipelineStats::ParseSpec(value);
	// "all" would drop every draw; an empty prefix is never a useful skip.
	std::erase_if(prefixes, [](const std::string& prefix) { return prefix.empty(); });
	return prefixes;
}

std::string JoinPrefixes(const std::vector<std::string>& prefixes) {
	std::string text;
	for (const auto& prefix: prefixes) {
		text += text.empty() ? prefix : "," + prefix;
	}
	return text;
}

std::string ReadFile() {
	std::FILE* file = std::fopen(kFileName, "rb");
	if (file == nullptr) {
		return {};
	}
	std::string text(kMaxFileBytes, '\0');
	const auto  size = std::fread(text.data(), 1, text.size(), file);
	std::fclose(file);
	text.resize(size);
	return text;
}

// Snapshots are immutable and published by pointer; the few replaced ones (one per manual edit
// of the file) are kept alive so readers never see a freed snapshot.
std::atomic<const Snapshot*> g_current {nullptr};
std::atomic<int64_t>         g_next_poll_ns {0};
std::mutex                   g_poll_mutex;
int64_t                      g_last_mtime_ns = -1;

const Snapshot& Empty() {
	static const Snapshot empty {};
	return empty;
}

int64_t ModifiedNs() {
	struct stat info {};
	if (stat(kFileName, &info) != 0) {
		return 0;
	}
#if defined(__APPLE__)
	return static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1'000'000'000 +
	       info.st_mtimespec.tv_nsec;
#else
	return static_cast<int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
#endif
}

void Poll() {
	const auto now  = NowNs();
	auto       next = g_next_poll_ns.load(std::memory_order_relaxed);
	if (now < next ||
	    !g_next_poll_ns.compare_exchange_strong(next, now + kPollInterval,
	                                            std::memory_order_relaxed)) {
		return;
	}
	std::scoped_lock lock(g_poll_mutex);
	const auto       mtime = ModifiedNs();
	if (mtime == g_last_mtime_ns) {
		return;
	}
	g_last_mtime_ns = mtime;
	const auto* snapshot = new Snapshot(mtime == 0 ? Snapshot {} : Parse(ReadFile()));
	const auto skip      = JoinPrefixes(snapshot->skip);
	const auto skip_draw = JoinPrefixes(snapshot->mesh_skip_draw);
	const auto skip_cs   = JoinPrefixes(snapshot->mesh_skip_cs);
	std::string ints;
	for (const auto& [name, number]: snapshot->ints) {
		ints += fmt::format(" {}={}", name, number);
	}
	LOGF("Live knobs: skip=[%s] mesh-skip-draw=[%s] mesh-skip-cs=[%s] marks=%d shake=%d%s\n", skip.c_str(),
	     skip_draw.c_str(), skip_cs.c_str(), snapshot->draw_marks ? 1 : 0, snapshot->shake_seconds, ints.c_str());
	std::fprintf(stderr, "[live-knobs] skip=[%s] mesh-skip-draw=[%s] mesh-skip-cs=[%s] marks=%d shake=%d%s\n",
	             skip.c_str(), skip_draw.c_str(), skip_cs.c_str(), snapshot->draw_marks ? 1 : 0,
	             snapshot->shake_seconds, ints.c_str());
	// Also in the perf log, so per-second [perf] lines can be split by knob phase.
	Common::PerfStats::EmitLine(fmt::format(
	    "[live-knobs] t={:.1f} skip=[{}] mesh-skip-draw=[{}] mesh-skip-cs=[{}] marks={}{}",
	    static_cast<double>(now) / 1e6, skip, skip_draw, skip_cs, snapshot->draw_marks ? 1 : 0, ints));
	if (snapshot->shake_seconds > 0 && !Libs::Controller::StartSyntheticShake(snapshot->shake_seconds)) {
		LOGF("Live knobs: shake ignored (already shaking or no pad)\n");
	}
	if (!snapshot->pad_script.empty()) {
		(void)Libs::Controller::StartPadScript(snapshot->pad_script);
	}
	g_current.store(snapshot, std::memory_order_release);
}

} // namespace

Snapshot Parse(std::string_view text) {
	Snapshot snapshot;
	while (!text.empty()) {
		const auto end  = std::min(text.find('\n'), text.size());
		const auto line = Trim(text.substr(0, end));
		text.remove_prefix(std::min(end + 1, text.size()));
		const auto equals = line.find('=');
		if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
			continue;
		}
		const auto key   = Trim(line.substr(0, equals));
		const auto value = Trim(line.substr(equals + 1));
		if (key == "skip") {
			snapshot.skip = ParsePrefixes(value);
		} else if (key == "mesh-skip-draw") {
			snapshot.mesh_skip_draw = ParsePrefixes(value);
		} else if (key == "mesh-skip-cs") {
			snapshot.mesh_skip_cs = ParsePrefixes(value);
		} else if (key == "marks") {
			snapshot.draw_marks = value == "1";
		} else if (key == "pad") {
			snapshot.pad_script = std::string(value);
		} else if (key == "shake") {
			int seconds = 0;
			const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
			if (error == std::errc() && ptr == value.data() + value.size() && seconds > 0) {
				snapshot.shake_seconds = std::min(seconds, kMaxShakeSeconds);
			}
		} else {
			int64_t number = 0;
			const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), number);
			if (!key.empty() && error == std::errc() && ptr == value.data() + value.size()) {
				snapshot.ints.emplace_back(std::string(key), number);
			}
		}
	}
	return snapshot;
}

const Snapshot& Current() {
	if (!Enabled()) {
		return Empty();
	}
	Poll();
	const auto* snapshot = g_current.load(std::memory_order_acquire);
	return snapshot != nullptr ? *snapshot : Empty();
}

namespace {

struct TitleWorkaround {
	const char* title_id;
	const char* lines;
	const char* reason;
};

// Astro Bot (PPSA21567): the crash-landing cinematic loses the device on Adreno 830 / Turnip in
// about half the runs. Dropping the draw of mesh shader 2b3be82b (the falling ship's particle
// trail; its compute stage still runs) avoids it; the shader is not used in gameplay, so the
// visual cost is that trail during that one cinematic.
constexpr TitleWorkaround kTitleWorkarounds[] = {
    {"PPSA21567", "mesh-skip-draw=2b3be82b",
     "Astro Bot: VK_ERROR_DEVICE_LOST in the crash-landing cinematic on Adreno 830"},
};

bool Matches(const std::vector<std::string>& prefixes, uint64_t hash) {
	return !prefixes.empty() && PipelineStats::Matches(prefixes, hash);
}

} // namespace

const Snapshot& TitleWorkarounds() {
	static const Snapshot snapshot = [] {
		const char* value = std::getenv("KYTY_TITLE_WORKAROUNDS");
		if (value == nullptr || std::strcmp(value, "1") != 0) {
			return Snapshot {};
		}
		std::string title_id;
		if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id)) {
			LOGF("Title workarounds: no TITLE_ID, none applied\n");
			return Snapshot {};
		}
		for (const auto& workaround: kTitleWorkarounds) {
			if (title_id == workaround.title_id) {
				LOGF("Title workarounds: %s: %s (%s)\n", workaround.title_id, workaround.lines,
				     workaround.reason);
				return Parse(workaround.lines);
			}
		}
		LOGF("Title workarounds: none for %s\n", title_id.c_str());
		return Snapshot {};
	}();
	return snapshot;
}

bool SkipShader(uint64_t guest_hash) {
	return Matches(Current().skip, guest_hash) || Matches(TitleWorkarounds().skip, guest_hash);
}

bool SkipMeshDraw(uint64_t mesh_hash) {
	return Matches(Current().mesh_skip_draw, mesh_hash) ||
	       Matches(TitleWorkarounds().mesh_skip_draw, mesh_hash);
}

bool SkipMeshDispatch(uint64_t mesh_hash) {
	return Matches(Current().mesh_skip_cs, mesh_hash) ||
	       Matches(TitleWorkarounds().mesh_skip_cs, mesh_hash);
}

bool DrawMarks() {
	return Current().draw_marks;
}

int64_t Int(std::string_view name, int64_t fallback) {
	for (const auto& [key, number]: Current().ints) {
		if (key == name) {
			return number;
		}
	}
	return fallback;
}

} // namespace Libs::Graphics::LiveKnobs
