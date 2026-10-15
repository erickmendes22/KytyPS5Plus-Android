#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_

#include "common/abi.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "loader/guestAbiInvoke.h"
#include "loader/timer.h" // IWYU pragma: keep

#include <fmt/format.h>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLED g_print_name

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_DEFINE(name) void name(Loader::SymbolDatabase* s)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_NAME(l, m)                                                                             \
	[[maybe_unused]] static thread_local bool PRINT_NAME_ENABLED = false;                          \
	static constexpr char                     g_library[]        = l;                              \
	static constexpr char                     g_module[]         = m;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_VERSION(l, lv, m, mv1, mv2)                                                            \
	LIB_NAME(l, m);                                                                                \
	static constexpr int g_library_version      = lv;                                              \
	static constexpr int g_module_version_major = mv1;                                             \
	static constexpr int g_module_version_minor = mv2;
// Guest-callable entry for a native HLE function f: its marshaling thunk on ARM64 (see
// guestAbiInvoke.h), nullptr on x86 hosts where guest code runs natively.
#if KYTY_HOST_ARM64
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_HLE_THUNK(f) Loader::GuestAbi::HleThunkFor<f>()
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_HLE_THUNK(f) nullptr
#endif

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD_THUNK(n, f, t, thunk)                                                              \
	{                                                                                              \
		Loader::SymbolResolve sr {};                                                               \
		sr.name                 = n;                                                               \
		sr.library              = g_library;                                                       \
		sr.library_version      = g_library_version;                                               \
		sr.module               = g_module;                                                        \
		sr.module_version_major = g_module_version_major;                                          \
		sr.module_version_minor = g_module_version_minor;                                          \
		sr.type                 = t;                                                               \
		auto        func        = reinterpret_cast<uint64_t>(f);                                   \
		const char* dbg_name    = "" #f;                                                           \
		s->Add(sr, func, dbg_name, thunk);                                                         \
	}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD(n, f, t) LIB_ADD_THUNK(n, f, t, nullptr)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_OBJECT(n, f) LIB_ADD(n, f, Loader::SymbolType::Object)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC(n, f) LIB_ADD_THUNK(n, f, Loader::SymbolType::Func, LIB_HLE_THUNK(f))
// Function whose guest entry is a hand-written thunk `thunk` (for functions that opt out of the
// generic marshaler, such as variadic ones). Like LIB_HLE_THUNK, the thunk exists only on ARM64.
#if KYTY_HOST_ARM64
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC_BOUND(n, f, thunk) LIB_ADD_THUNK(n, f, Loader::SymbolType::Func, thunk)
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC_BOUND(n, f, thunk) LIB_ADD_THUNK(n, f, Loader::SymbolType::Func, nullptr)
#endif

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME()                                                                               \
	if (PRINT_NAME_ENABLED) {                                                                      \
		Libs::PrintName(g_library, g_module, __func__);                                              \
	}

namespace Loader {
class SymbolDatabase;
} // namespace Loader

namespace Libs {

// Keep the formatting path from inflating fiber functions' stack frames under LTO.
[[gnu::noinline]] inline void PrintName(const char* library, const char* module, const char* function) {
	if (Log::GetDirection() != Log::Direction::Silent) {
		const auto elapsed_ms      = static_cast<uint64_t>(Loader::Timer::GetTimeMs());
		const auto print_name_time = fmt::format(
		    "{:02}:{:02}:{:02}.{:03}", elapsed_ms / 3600000, (elapsed_ms / 60000) % 60,
		    (elapsed_ms / 1000) % 60, elapsed_ms % 1000);
		LOGF_COLOR(Log::Color::Cyan, "[%d][%s] %s::%s::%s()\n",
		           Common::Thread::GetThreadIdUnique(), print_name_time.c_str(), library, module,
		           function);
	}
}

void InitAll(Loader::SymbolDatabase* s);

} // namespace Libs
#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_ */
