#ifndef KYTY_COMMON_PROCESSEXITHOOKS_H_
#define KYTY_COMMON_PROCESSEXITHOOKS_H_

// Hooks that undo process-external state (e.g. a device-global GPU clock request) on every exit
// path the emulator can see: exit(), quick_exit(), Common::DbgExit (EXIT macros, GpuFatal), the
// page manager's fail-fast exits, an unresolved host fault, and SIGABRT/SIGTERM/SIGINT. A
// SIGKILL (Android force-stop, low-memory killer) runs nothing; callers must also reset such
// state at the next start.

namespace Common {

// Must be idempotent and async-signal-safe (plain syscalls, atomics; no locks, no allocation):
// hooks can run from a signal handler and more than once.
using ProcessExitHook = void (*)() noexcept;

// Registers a hook (at most 8; returns false when full). The first call also registers
// RunProcessExitHooks with atexit/at_quick_exit and installs the termination signal handlers,
// which chain to the previous handler (e.g. Android's crash reporter) afterwards.
bool AddProcessExitHook(ProcessExitHook hook);

// Runs every registered hook. Async-signal-safe.
void RunProcessExitHooks() noexcept;

} // namespace Common

#endif /* KYTY_COMMON_PROCESSEXITHOOKS_H_ */
