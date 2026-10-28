#define KYTY_SIGNAL_ORIGIN_TEST 1
#include "common/signalOrigin.h"
#include <sys/wait.h>
#include <thread>
#include <fstream>
#include <string>
#include <cassert>

int main() {
    // Run each installation in its own process: seccomp is intentionally irreversible.
    for (int which = 0; which < 9; ++which) {
        const auto path = "/tmp/kyty-signal-test-" + std::to_string(which) + ".txt";
        int ready[2]; assert(pipe(ready) == 0);
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            close(ready[0]);
            // A pre-existing thread proves TSYNC, rather than inheritance alone.
            std::thread existing([&] {
                char c; (void)c;
                while (!Common::SignalOrigin::probes.load(std::memory_order_acquire)) sched_yield();
                if (which == 6) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGKILL);
            });
            assert(Common::SignalOrigin::Install(path.c_str()));
            if (which == 6) { existing.join(); _exit(4); }
            existing.join();
            switch (which) {
            case 0: syscall(SYS_kill, getpid(), SIGKILL); break;
            case 1: syscall(SYS_tkill, syscall(SYS_gettid), SIGKILL); break;
            case 2: syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGKILL); break;
            case 3: syscall(SYS_rt_sigqueueinfo, getpid(), SIGKILL, nullptr); break;
            case 4: syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), SIGKILL, nullptr); break;
            case 5: syscall(SYS_pidfd_send_signal, -1, SIGKILL, nullptr, 0); break;
            case 7: std::thread([] { syscall(SYS_kill, getpid(), SIGKILL); }).join(); break;
            case 8: { char c = 'R'; write(ready[1], &c, 1); for (;;) pause(); }
            }
            _exit(5);
        }
        close(ready[1]);
        if (which == 8) { char c; assert(read(ready[0], &c, 1) == 1); assert(kill(child, SIGKILL) == 0); }
        int status = 0; assert(waitpid(child, &status, 0) == child);
        std::ifstream in(path); std::string log((std::istreambuf_iterator<char>(in)), {});
        assert(log.find("PROBE_PASS") != std::string::npos);
        if (which == 8) {
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            assert(log.find("\nINTERNAL_SIGKILL_ATTEMPT") == std::string::npos);
        } else {
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 190);
            assert(log.find("\nINTERNAL_SIGKILL_ATTEMPT") != std::string::npos);
        }
        close(ready[0]);
    }
    puts("PASS: six kill syscall forms, existing/new thread coverage, external SIGKILL distinction");
}
