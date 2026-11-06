#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
namespace Cpu::DecodeDiagnostics {
inline bool AddressFromMessage(const char* message, uint64_t& address) {
    constexpr const char* marker = " instruction in entry block: ";
    const char* start = std::strstr(message, marker);
    if (!start) return false;
    start += std::strlen(marker);
    const auto result = std::from_chars(start, start + std::strlen(start), address, 16);
    return result.ec == std::errc{} && result.ptr != start && address != 0;
}
// Kernel-mediated copy: logging an invalid RIP must not cause a second SIGSEGV.
inline size_t ReadSelf(uint64_t address, void* destination, size_t size) {
#if defined(__linux__)
    iovec local{destination, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    const auto read = ::syscall(SYS_process_vm_readv, ::getpid(), &local, 1, &remote, 1, 0);
    if (read > 0) return static_cast<size_t>(read);
    // Some Android policies disable process_vm_readv even for self.
    const int fd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const auto copied = ::pread(fd, destination, size, static_cast<off_t>(address));
    ::close(fd);
    return copied > 0 ? static_cast<size_t>(copied) : 0;
#else
    (void)address; (void)destination; (void)size;
    return 0;
#endif
}
inline void Log(uint64_t rip) {
    std::array<unsigned char, 32> bytes{};
    const auto size = ReadSelf(rip, bytes.data(), bytes.size());
    std::fprintf(stderr, "\n[FEX_DECODE] guest_rip=%016llx readable=%zu bytes=",
        static_cast<unsigned long long>(rip), size);
    for (size_t i = 0; i < size; ++i) std::fprintf(stderr, "%02x", bytes[i]);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}
} // namespace Cpu::DecodeDiagnostics
