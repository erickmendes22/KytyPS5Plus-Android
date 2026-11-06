#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/mman.h>

static void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
#if defined(__x86_64__)
    if (!__builtin_cpu_supports("avx")) {
        std::puts("Demon geometry guest-branch fixture skipped: AVX host required");
        return 0;
    }
    // Exact guest instructions at image+0xA0D6FB..0xA0D728 in the supplied
    // PPSA01341 eboot (SELF executable file base 0xFE50). No relocated operands.
    constexpr uint8_t guest[] = {
        0x48,0x8b,0x49,0x68,0x80,0xb9,0xaa,0x0c,0x00,0x00,0x01,0x75,0x20,
        0x41,0xc6,0x86,0xec,0xda,0x02,0x00,0x01,0x48,0xc7,0x03,0x00,0x00,
        0x00,0x00,0xc5,0xf8,0x10,0x80,0xf0,0x0d,0x00,0x00,0xc4,0xc1,0x78,
        0x11,0x86,0xb8,0xda,0x02,0x00,
    };
    constexpr uint8_t enter[] = {
        0x53,                   // push rbx
        0x41,0x56,              // push r14
        0x48,0x89,0xd3,         // mov rbx,rdx (index count/result)
        0x49,0x89,0xf6,         // mov r14,rsi (render state)
        0x48,0x89,0xc8,         // mov rax,rcx (source constants)
        0x48,0x89,0xf9,         // mov rcx,rdi (outer render args)
    };
    constexpr uint8_t leave[] = {0xc5,0xf8,0x77,0x41,0x5e,0x5b,0xc3};
    auto* code = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(code != MAP_FAILED, "fixture mapping failed");
    std::memcpy(code, enter, sizeof(enter));
    std::memcpy(code + sizeof(enter), guest, sizeof(guest));
    std::memcpy(code + sizeof(enter) + sizeof(guest), leave, sizeof(leave));
    Check(mprotect(code, 4096, PROT_READ | PROT_EXEC) == 0, "fixture protect failed");
    using Fixture = void (*)(void*, void*, void*, void*);
    auto fixture = reinterpret_cast<Fixture>(code);
    std::array<uint8_t, 0xd00> cvars {};
    std::array<uint8_t, 0x80> outer {};
    auto* cvar_pointer = cvars.data();
    std::memcpy(outer.data() + 0x68, &cvar_pointer, sizeof(cvar_pointer));
    std::vector<uint8_t> render(0x2db00), source(0xe10, 0xa5);
    for (const uint8_t enabled: {0, 1}) {
        std::fill(render.begin(), render.end(), 0);
        uint64_t index_result = 63;
        cvars[0xcaa] = enabled;
        fixture(outer.data(), render.data(), &index_result, source.data());
        Check(render[0x2daec] == enabled, "wrong GPU-cull path flag");
        Check(index_result == (enabled ? 0u : 63u), "unculled path did not preserve original index result");
        if (enabled) Check(render[0x2dab8] == 0xa5, "GPU-cull replacement was not selected");
    }
    Check(munmap(code, 4096) == 0, "fixture cleanup failed");
    std::puts("Demon geometry: actual guest branch preserves original indices with culling off; GPU-cull path zeroes result before its job");
#endif
}
