#include "kernel/sparseBackingRead.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

using Libs::LibKernel::Memory::SparseBackingRead::Read;
static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static uint64_t Allocated(int fd) {
    struct stat st {};
    Check(fstat(fd, &st) == 0, "stat failed");
    return static_cast<uint64_t>(st.st_blocks) * 512;
}
int main() {
    constexpr uint64_t offset = 0x110000000ull, size = 64ull * 1024 * 1024;
    const int fd = memfd_create("KytySparseReadTest", 0);
    Check(fd >= 0 && ftruncate(fd, offset + size) == 0, "memfd create failed");
    auto* view = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                          fd, offset));
    Check(view != MAP_FAILED, "map failed");
    constexpr std::array<uint64_t, 3> positions {0, 16ull * 1024 * 1024 + 17, size - 1};
    constexpr std::array<uint8_t, 3> values {0x39, 0x72, 0xa5};
    for (size_t i = 0; i < positions.size(); ++i) view[positions[i]] = values[i];
    const auto before = Allocated(fd);
    std::vector<uint8_t> output(size + 2, 0x55);
    for (const auto prefix: {0u, 37u}) {
        const uint64_t length = size - prefix - 23;
        Check(Read(fd, output.data() + 1, offset + prefix, length, offset + size), "read failed");
        for (uint64_t i = 0; i < length; ++i) {
            uint8_t expected = 0;
            for (size_t k = 0; k < positions.size(); ++k)
                if (i + prefix == positions[k]) expected = values[k];
            if (output[i + 1] != expected) Check(false, "wrong bytes or file offset");
        }
        Check(output.front() == 0x55 && output[length + 1] == 0x55, "canary overwritten");
        Check(Allocated(fd) == before, "sparse read allocated file holes");
        std::fill(output.begin(), output.end(), 0x55);
    }
    Check(!Read(fd, output.data(), offset + size, 1, offset + size), "accepted past EOF");
    Check(!Read(-1, output.data(), 0, 1, 1), "accepted invalid descriptor");
    Check(!Read(fd, nullptr, 0, 1, offset + size), "accepted null output");
    // Absolute metadata seeks and preads must remain correct with concurrent readers.
    std::atomic<bool> correct {true};
    std::vector<std::thread> readers;
    for (unsigned k = 0; k < 4; ++k) readers.emplace_back([&] {
        std::array<uint8_t, 8192> bytes {};
        for (unsigned round = 0; round < 32; ++round) {
            if (!Read(fd, bytes.data(), offset + positions[1] - 37, bytes.size(), offset + size) ||
                bytes[37] != values[1]) correct.store(false);
        }
    });
    for (auto& reader: readers) reader.join();
    Check(correct.load() && Allocated(fd) == before, "concurrent read changed data or residency");
    // Observe a later MAP_SHARED update, rather than caching an obsolete hole map.
    view[4096 * 7 + 19] = 0xb6;
    Check(Read(fd, output.data(), offset + 4096 * 7, 4096, offset + size) && output[19] == 0xb6,
          "missed later mapped write");
    Check(munmap(view, size) == 0 && close(fd) == 0, "cleanup failed");
    std::printf("Sparse backing: exact bytes, >4GiB offsets, holes, bounds, aliases and concurrent reads passed\n");
}
