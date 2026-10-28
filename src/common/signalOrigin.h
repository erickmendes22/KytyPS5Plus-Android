#pragma once

// Experimental Android SIGKILL provenance probe. This is a syscall filter, not
// a SIGKILL handler. External SIGKILL cannot be intercepted by the victim.
#if defined(KYTY_ANDROID) || defined(KYTY_SIGNAL_ORIGIN_TEST)
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace Common::SignalOrigin {
inline int fd = -1;
inline struct sigaction previous {};
inline constexpr unsigned Marker = 0x4b59;
inline std::atomic<int> probes {0};
static_assert(std::atomic<int>::is_always_lock_free);
inline int stack_fd = -1;

// Signal context: no stdio, unwinder, allocation, mutex or pipe. One small write
// goes directly to the app-private O_DSYNC file, independently of session.log.
inline void Record(const char* label, const siginfo_t* info, void* context) {
    char out[1024];
    size_t n = 0;
    auto text = [&](const char* s) { while (*s && n < sizeof(out)-1) out[n++] = *s++; };
    auto hex = [&](uint64_t v) {
        text("0x");
        for (int i = 60; i >= 0; i -= 4) out[n++] = "0123456789abcdef"[(v >> i) & 15];
    };
    text(label); text(" pid="); hex(getpid()); text(" tid="); hex(syscall(SYS_gettid));
    text(" syscall="); hex(info->si_syscall); text(" call="); hex(reinterpret_cast<uintptr_t>(info->si_call_addr));
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    text(" pc="); hex(uc->uc_mcontext.pc); text(" lr="); hex(uc->uc_mcontext.regs[30]);
    text(" fp="); hex(uc->uc_mcontext.regs[29]); text(" sp="); hex(uc->uc_mcontext.sp);
    for (unsigned i = 0; i < 6; ++i) { text(" arg="); hex(uc->uc_mcontext.regs[i]); }
#elif defined(__x86_64__)
    text(" pc="); hex(uc->uc_mcontext.gregs[REG_RIP]);
    text(" sp="); hex(uc->uc_mcontext.gregs[REG_RSP]);
    for (int r : {REG_RDI, REG_RSI, REG_RDX, REG_R10, REG_R8, REG_R9}) {
        text(" arg="); hex(uc->uc_mcontext.gregs[r]);
    }
#endif
    text("\n");
    if (fd >= 0) (void)::write(fd, out, n);
}

inline void RecordGuest(uint64_t rip, const uint64_t* regs, size_t count) {
    char out[512];
    size_t n = 0;
    const char label[] = "FEX_SAVED_GUEST_STATE rip=";
    for (char c : label) if (c) out[n++] = c;
    auto hex = [&](uint64_t v) {
        out[n++] = '0'; out[n++] = 'x';
        for (int i = 60; i >= 0; i -= 4) out[n++] = "0123456789abcdef"[(v >> i) & 15];
    };
    hex(rip);
    for (size_t i = 0; i < count && i < 16; ++i) {
        out[n++] = ' '; hex(regs[i]);
    }
    out[n++] = '\n';
    if (fd >= 0) (void)write(fd, out, n);
}

inline bool Handle(int sig, void* raw_info, void* context) {
    auto* info = static_cast<siginfo_t*>(raw_info);
    if (sig != SIGSYS || !info || info->si_code != 1 || info->si_errno != Marker) return false;
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    const auto signal = uc->uc_mcontext.regs[1];
#elif defined(__x86_64__)
    const auto signal = uc->uc_mcontext.gregs[REG_RSI];
#endif
    // Only kill(self, 0) is the harmless self-test. All other traps are signal 9.
    if (info->si_syscall == SYS_kill && signal == 0) {
        Record("PROBE_PASS", info, context);
        probes.store(1, std::memory_order_release);
#if defined(__aarch64__)
        uc->uc_mcontext.regs[0] = 0;
#elif defined(__x86_64__)
        uc->uc_mcontext.gregs[REG_RAX] = 0;
#endif
        return true;
    }
    Record("INTERNAL_SIGKILL_ATTEMPT", info, context);
#if defined(__aarch64__)
    const auto stack = uc->uc_mcontext.sp;
#elif defined(__x86_64__)
    const auto stack = uc->uc_mcontext.gregs[REG_RSP];
#endif
    // The kernel copies a bounded raw stack; invalid pages produce EFAULT,
    // instead of recursively faulting by dereferencing frame pointers here.
    if (stack_fd >= 0) (void)write(stack_fd, reinterpret_cast<const void*>(stack), 4096);
    // Preserve causality: stop at the attempted kill instead of returning to
    // possibly broken code. 190 deliberately differs from signal 9.
    _exit(190);
}

inline void Handler(int sig, siginfo_t* info, void* context) {
    if (Handle(sig, info, context)) return;
    if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction) {
        previous.sa_sigaction(sig, info, context);
    } else if (previous.sa_handler == SIG_IGN) {
        return;
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler) {
        previous.sa_handler(sig);
    } else {
        sigaction(sig, &previous, nullptr);
        raise(sig);
    }
}

inline void SnapshotMaps(const char* destination) {
    int in = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    int out = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (in >= 0 && out >= 0) {
        char buffer[4096];
        ssize_t size;
        while ((size = read(in, buffer, sizeof(buffer))) > 0) {
            ssize_t offset = 0;
            while (offset < size) {
                auto written = write(out, buffer + offset, size - offset);
                if (written <= 0) break;
                offset += written;
            }
        }
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}

inline bool Install(const char* path) {
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_DSYNC, 0600);
    if (fd < 0) return false;
    char stack_path[4096];
    if (snprintf(stack_path, sizeof(stack_path), "%s.stack", path) < static_cast<int>(sizeof(stack_path)))
        stack_fd = open(stack_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_DSYNC, 0600);
    dprintf(fd, "SIGKILL syscall provenance v1 pid=%d\n", getpid());
    struct sigaction action {};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSYS, &action, &previous)) return false;
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGSYS);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    std::vector<sock_filter> code;
    auto stmt = [&](unsigned short op, unsigned k) { code.push_back(BPF_STMT(op, k)); };
    auto jump = [&](unsigned short op, unsigned k, unsigned char yes, unsigned char no) {
        code.push_back(BPF_JUMP(op, k, yes, no));
    };
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
#if defined(__aarch64__)
    jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
#elif defined(__x86_64__)
    jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
#endif
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    // Reload nr for each block: inspecting an argument overwrites BPF A.
    auto signal_call = [&](unsigned nr, unsigned arg) {
        stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
        jump(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 3);
        stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args) + arg * 8);
        jump(BPF_JMP | BPF_JEQ | BPF_K, SIGKILL, 0, 1);
        stmt(BPF_RET | BPF_K, SECCOMP_RET_TRAP | Marker);
    };
    signal_call(SYS_kill, 1);
    signal_call(SYS_tkill, 1);
    signal_call(SYS_tgkill, 2);
    signal_call(SYS_rt_sigqueueinfo, 1);
    signal_call(SYS_rt_tgsigqueueinfo, 2);
#ifdef SYS_pidfd_send_signal
    signal_call(SYS_pidfd_send_signal, 1);
#endif
    // Safe proof that the live filter and handler work, without ever sending 9.
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
    jump(BPF_JMP | BPF_JEQ | BPF_K, SYS_kill, 0, 5);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args));
    jump(BPF_JMP | BPF_JEQ | BPF_K, getpid(), 0, 3);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args) + 8);
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_TRAP | Marker);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    sock_fprog program {static_cast<unsigned short>(code.size()), code.data()};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        dprintf(fd, "INSTALL_FAILED no_new_privs errno=%d\n", errno); return false;
    }
    const long result = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program);
    if (result != 0) {
        dprintf(fd, "INSTALL_FAILED tsync=%ld errno=%d (coverage NOT established)\n", result, errno);
        return false;
    }
    syscall(SYS_kill, getpid(), 0);
    dprintf(fd, "COVERAGE tsync=all-current-threads future-threads=inherit probe=%d\n", probes.load(std::memory_order_acquire));
    dprintf(fd, "If exit is still signal 9 without an internal attempt record, origin is outside covered syscalls; sender identity is not available here.\n");
    return probes.load(std::memory_order_acquire) == 1;
}
} // namespace Common::SignalOrigin
#endif
