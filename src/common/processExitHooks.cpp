#include "common/processExitHooks.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#if !defined(_WIN32)
#include <csignal>
#include <signal.h>
#endif

namespace Common {

namespace {

constexpr std::size_t MaxHooks = 8;

std::array<std::atomic<ProcessExitHook>, MaxHooks> g_hooks {};
std::atomic<std::size_t>                            g_hook_count {0};

void RunHooksAtExit() {
	RunProcessExitHooks();
}

#if !defined(_WIN32)
// SIGQUIT is left alone: Android's runtime uses it for stack dumps.
constexpr std::array<int, 3> TerminationSignals = {SIGABRT, SIGTERM, SIGINT};

std::array<struct sigaction, TerminationSignals.size()> g_previous {};

void OnTerminationSignal(int signal_number, siginfo_t* info, void* context) {
	RunProcessExitHooks();
	for (std::size_t i = 0; i < TerminationSignals.size(); i++) {
		if (TerminationSignals[i] != signal_number) {
			continue;
		}
		const auto& previous = g_previous[i];
		if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
			previous.sa_sigaction(signal_number, info, context);
			return;
		}
		if (previous.sa_handler == SIG_IGN) {
			return;
		}
		if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
			previous.sa_handler(signal_number);
			return;
		}
		// Default action: restore it and re-raise; the signal is delivered once this returns.
		::sigaction(signal_number, &previous, nullptr);
		::raise(signal_number);
		return;
	}
}

void InstallTerminationHandlers() {
	struct sigaction action {};
	action.sa_sigaction = OnTerminationSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (std::size_t i = 0; i < TerminationSignals.size(); i++) {
		if (::sigaction(TerminationSignals[i], &action, &g_previous[i]) != 0) {
			g_previous[i]            = {};
			g_previous[i].sa_handler = SIG_DFL;
		}
	}
}
#endif

} // namespace

bool AddProcessExitHook(ProcessExitHook hook) {
	if (hook == nullptr) {
		return false;
	}
	static std::once_flag s_installed;
	std::call_once(s_installed, [] {
		(void)std::atexit(RunHooksAtExit);
		(void)std::at_quick_exit(RunHooksAtExit);
#if !defined(_WIN32)
		InstallTerminationHandlers();
#endif
	});
	const auto slot = g_hook_count.fetch_add(1, std::memory_order_acq_rel);
	if (slot >= MaxHooks) {
		g_hook_count.store(MaxHooks, std::memory_order_release);
		return false;
	}
	g_hooks[slot].store(hook, std::memory_order_release);
	return true;
}

void RunProcessExitHooks() noexcept {
	const auto count = g_hook_count.load(std::memory_order_acquire);
	for (std::size_t i = 0; i < count && i < MaxHooks; i++) {
		if (const auto hook = g_hooks[i].load(std::memory_order_acquire); hook != nullptr) {
			hook();
		}
	}
}

} // namespace Common
