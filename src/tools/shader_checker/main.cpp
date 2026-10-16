// shader_checker: rebuilds the pipelines that `kyty --dump-shaders <dir>` recorded on this
// device's Vulkan driver and reports every one the driver rejects, crashes on, or hangs on.
//
//   shader_checker [options] <dump-dir>
//   shader_checker [options] --record <dump-dir>/pipelines/<name>.json [--replace 0=cand.spv]
//
// Exit status: 0 every pipeline compiled, 1 at least one failed/crashed/timed out, 2 usage or
// device setup error. Records are checked in forked workers so a driver crash or hang only costs
// the record that caused it.

#include "graphics/shader/deviceQuirks.h"
#include "tools/shader_checker/checkerPipeline.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fmt/format.h>
#include <map>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ShaderChecker {
namespace {

struct Args {
	CheckOptions                       check;
	std::vector<std::filesystem::path> records;
	std::string                        filter;
	int                                gpu_index       = -1;
	int                                timeout_seconds = 180;
	bool                               isolate         = true;
	bool                               quiet           = false;
};

void Usage() {
	std::puts(
	    "usage: shader_checker [options] <dump-dir>\n"
	    "       shader_checker [options] --record <file.json> [--record ...]\n"
	    "  --root <dir>          dump root for --record (default: the record's ../..)\n"
	    "  --filter <text>       only records whose file name contains <text>\n"
	    "  --replace <i>=<spv>   use <spv> for stage <i> of every checked record\n"
	    "  --validate            run spirv-val on each module first\n"
	    "  --timeout <seconds>   per-pipeline limit before the worker is killed (default 180)\n"
	    "  --no-isolate          check in this process (a driver crash ends the run)\n"
	    "  --gpu <index>         physical device index\n"
	    "  --quiet               print failures and the summary only");
}

bool ParseArgs(int argc, char** argv, Args& args) {
	std::filesystem::path dir;
	for (int i = 1; i < argc; i++) {
		const std::string arg  = argv[i];
		const auto        next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
		if (arg == "--record" || arg == "--root" || arg == "--filter" || arg == "--replace" ||
		    arg == "--timeout" || arg == "--gpu") {
			const char* value = next();
			if (value == nullptr) {
				fmt::print(stderr, "{} needs a value\n", arg);
				return false;
			}
			if (arg == "--record") {
				args.records.emplace_back(value);
			} else if (arg == "--root") {
				args.check.root = value;
			} else if (arg == "--filter") {
				args.filter = value;
			} else if (arg == "--timeout") {
				args.timeout_seconds = std::max(1, std::atoi(value));
			} else if (arg == "--gpu") {
				args.gpu_index = std::atoi(value);
			} else {
				const std::string replace = value;
				const auto        eq      = replace.find('=');
				if (eq == std::string::npos) {
					fmt::print(stderr, "--replace expects <stage-index>=<file>\n");
					return false;
				}
				args.check.replacements[std::strtoul(replace.substr(0, eq).c_str(), nullptr, 10)] =
				    replace.substr(eq + 1);
			}
		} else if (arg == "--validate") {
			args.check.validate = true;
		} else if (arg == "--no-isolate") {
			args.isolate = false;
		} else if (arg == "--quiet") {
			args.quiet = true;
		} else if (arg == "--help" || arg == "-h" || arg.starts_with("--") || !dir.empty()) {
			return false;
		} else {
			dir = arg;
		}
	}
	if (!dir.empty()) {
		if (args.check.root.empty()) args.check.root = dir;
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir / "pipelines", ec)) {
			if (entry.path().extension() == ".json") args.records.push_back(entry.path());
		}
		if (ec) {
			fmt::print(stderr, "cannot list {}: {}\n", (dir / "pipelines").string(), ec.message());
			return false;
		}
	}
	if (args.records.empty() && dir.empty()) {
		return false;
	}
	if (args.check.root.empty()) {
		args.check.root = args.records.front().parent_path().parent_path();
	}
	std::erase_if(args.records, [&](const auto& path) {
		return path.filename().string().find(args.filter) == std::string::npos;
	});
	std::ranges::sort(args.records);
	return true;
}

const char* StatusName(Status status) {
	switch (status) {
		case Status::Ok: return "OK";
		case Status::Fail: return "FAIL";
		case Status::Skip: return "SKIP";
	}
	return "?";
}

// One finished record as reported by a worker (or synthesized for a crash/timeout).
struct Line {
	std::string status;
	std::string reason;
	std::string guests;
	double      milliseconds = 0.0;
	size_t      words        = 0;
};

std::string Encode(size_t index, const CheckResult& r) {
	auto clean = r.reason;
	std::ranges::replace(clean, '\t', ' ');
	std::ranges::replace(clean, '\n', ' ');
	return fmt::format("{}\t{}\t{:.1f}\t{}\t{}\t{}\n", index, StatusName(r.status), r.milliseconds,
	                   r.spirv_words, r.guests, clean);
}

bool Decode(const std::string& text, size_t& index, Line& line) {
	std::vector<std::string> fields;
	size_t                   start = 0;
	for (size_t tab; (tab = text.find('\t', start)) != std::string::npos; start = tab + 1) {
		fields.push_back(text.substr(start, tab - start));
	}
	fields.push_back(text.substr(start));
	if (fields.size() != 6) return false;
	index             = std::strtoul(fields[0].c_str(), nullptr, 10);
	line.status       = fields[1];
	line.milliseconds = std::strtod(fields[2].c_str(), nullptr);
	line.words        = std::strtoul(fields[3].c_str(), nullptr, 10);
	line.guests       = fields[4];
	line.reason       = fields[5];
	return true;
}

[[noreturn]] void RunWorker(const Args& args, size_t first, int fd) {
	std::string error;
	const auto  device = Device::Create(args.gpu_index, &error);
	if (!device) {
		fmt::print(stderr, "device setup failed: {}\n", error);
		_exit(3);
	}
	for (size_t i = first; i < args.records.size(); i++) {
		const auto text = Encode(i, CheckRecord(*device, args.records[i], args.check));
		if (write(fd, text.data(), text.size()) < 0) _exit(4);
	}
	_exit(0);
}

class Runner {
public:
	explicit Runner(const Args& args): m_args(args), m_lines(args.records.size()) {}

	bool Run() {
		size_t next = 0;
		while (next < m_args.records.size()) {
			if (!RunBatch(next)) return false;
		}
		return true;
	}

	[[nodiscard]] const std::vector<Line>& Lines() const { return m_lines; }

private:
	// Runs one worker from `next`; returns false on a setup failure that would repeat.
	bool RunBatch(size_t& next) {
		int fds[2];
		if (pipe(fds) != 0) return false;
		const pid_t pid = fork();
		if (pid == 0) {
			close(fds[0]);
			RunWorker(m_args, next, fds[1]);
		}
		close(fds[1]);
		std::string buffer;
		bool        timed_out = false;
		for (;;) {
			pollfd poll_fd {fds[0], POLLIN, 0};
			const int ready = poll(&poll_fd, 1, m_args.timeout_seconds * 1000);
			if (ready == 0) {
				timed_out = true;
				kill(pid, SIGKILL);
				break;
			}
			if (ready < 0 && errno == EINTR) continue;
			char       chunk[4096];
			const auto n = read(fds[0], chunk, sizeof(chunk));
			if (n <= 0) break;
			buffer.append(chunk, static_cast<size_t>(n));
			for (size_t eol; (eol = buffer.find('\n')) != std::string::npos;) {
				size_t index = 0;
				Line   line;
				if (Decode(buffer.substr(0, eol), index, line) && index < m_lines.size()) {
					Report(index, std::move(line));
					next = index + 1;
				}
				buffer.erase(0, eol + 1);
			}
		}
		close(fds[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		if (next >= m_args.records.size()) return true;
		if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 3) {
			fmt::print(stderr, "worker could not create the Vulkan device\n");
			return false;
		}
		Line line;
		line.status = timed_out ? "TIMEOUT" : "CRASH";
		line.reason = timed_out ? fmt::format("no answer in {} s", m_args.timeout_seconds)
		              : WIFSIGNALED(status)
		                  ? fmt::format("driver killed the worker: {}", strsignal(WTERMSIG(status)))
		                  : fmt::format("worker exited with {}", WEXITSTATUS(status));
		Report(next, std::move(line));
		next++;
		return true;
	}

	void Report(size_t index, Line line) {
		if (!m_args.quiet || line.status != "OK") {
			fmt::print("{:<7} {}  {:.1f} ms  {} words  guest {}  {}\n", line.status,
			           m_args.records[index].filename().string(), line.milliseconds, line.words,
			           line.guests, line.status == "OK" ? "" : line.reason);
			std::fflush(stdout);
		}
		m_lines[index] = std::move(line);
	}

	const Args&       m_args;
	std::vector<Line> m_lines;
};

int Summarize(const Args& args, const std::vector<Line>& lines) {
	std::map<std::string, size_t> counts;
	for (const auto& line: lines) counts[line.status]++;
	fmt::print("\n{} pipelines:", lines.size());
	for (const auto& [status, count]: counts) fmt::print(" {} {}", status, count);
	fmt::print("\n");
	bool bad = false;
	for (size_t i = 0; i < lines.size(); i++) {
		if (lines[i].status == "OK" || lines[i].status == "SKIP") continue;
		if (!bad) fmt::print("problems:\n");
		bad = true;
		fmt::print("  {} {} guest {}: {}\n", lines[i].status, args.records[i].filename().string(),
		           lines[i].guests, lines[i].reason);
	}
	return bad ? 1 : 0;
}

int Main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}
	{
		// Header only: workers build their own device after fork.
		if (args.isolate) {
			const pid_t pid = fork();
			if (pid == 0) {
				std::string error;
				const auto  device = Device::Create(args.gpu_index, &error);
				if (!device) {
					fmt::print(stderr, "device setup failed: {}\n", error);
					_exit(3);
				}
				const auto& props  = device->Properties();
				const auto  quirks = Libs::Graphics::DetectShaderDeviceQuirks(
                    {.vendor_id      = props.vendorID,
				      .device_id      = props.deviceID,
				      .driver_id      = static_cast<uint32_t>(device->Driver().driverID),
				      .driver_version = props.driverVersion});
				fmt::print("device: {} | driver {} {} | emulator shader quirks: {} | {} records\n",
				           props.deviceName.data(), device->Driver().driverName.data(),
				           device->Driver().driverInfo.data(),
				           Libs::Graphics::DescribeShaderQuirks(quirks), args.records.size());
				std::fflush(stdout);
				_exit(0);
			}
			int status = 0;
			waitpid(pid, &status, 0);
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 2;
		}
	}
	if (!args.isolate) {
		std::string error;
		const auto  device = Device::Create(args.gpu_index, &error);
		if (!device) {
			fmt::print(stderr, "device setup failed: {}\n", error);
			return 2;
		}
		std::vector<Line> lines;
		for (const auto& record: args.records) {
			const auto r = CheckRecord(*device, record, args.check);
			Line       line {StatusName(r.status), r.reason, r.guests, r.milliseconds, r.spirv_words};
			if (!args.quiet || r.status != Status::Ok) {
				fmt::print("{:<7} {}  {:.1f} ms  {} words  guest {}  {}\n", line.status,
				           record.filename().string(), line.milliseconds, line.words, line.guests,
				           r.status == Status::Ok ? "" : line.reason);
			}
			lines.push_back(std::move(line));
		}
		return Summarize(args, lines);
	}
	Runner runner(args);
	if (!runner.Run()) return 2;
	return Summarize(args, runner.Lines());
}

} // namespace
} // namespace ShaderChecker

int main(int argc, char** argv) {
	return ShaderChecker::Main(argc, argv);
}
