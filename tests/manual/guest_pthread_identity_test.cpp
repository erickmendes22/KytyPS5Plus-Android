// Reproduce the Bink recursive-owner comparison captured in both PS5 EBOOTs.
// The guest reserves bit 63 for contention; its pthread identity must not use it.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
int main() {
    constexpr uint64_t tagged = 0xb400007acca0ae80ull; // Wolverine SDLThread R15
    constexpr uint64_t canonical = 0x0000007acca0ae80ull;
    constexpr uint64_t contended = 1ull << 63;
    const auto owns = [](uint64_t self, uint64_t owner) { return (owner & ~contended) == self; };
    assert(!owns(tagged, tagged));
    assert(owns(canonical, canonical));
    assert(owns(canonical, canonical | contended));
    assert(!owns(canonical, canonical + 16));
#if defined(__x86_64__) && defined(__linux__)
    if (__builtin_cpu_supports("bmi2")) {
        // mov al,63; bzhi rax,rsi,rax are the original EBOOT bytes at
        // Demon 0x1e6ec32 and Wolverine 0xe1a5e2. Compare to SysV self in RDI.
        constexpr std::array<uint8_t, 17> code{
            0xb0,0x3f,0xc4,0xe2,0xf8,0xf5,0xc6,0x48,0x39,0xf8,
            0x0f,0x94,0xc0,0x0f,0xb6,0xc0,0xc3};
        const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(page != MAP_FAILED);
        std::memcpy(page, code.data(), code.size());
        assert(mprotect(page, page_size, PROT_READ | PROT_EXEC) == 0);
        const auto compare = reinterpret_cast<uint64_t(*)(uint64_t,uint64_t)>(page);
        assert(compare(tagged, tagged) == 0);
        assert(compare(canonical, canonical) == 1);
        assert(compare(canonical, canonical | contended) == 1);
        assert(compare(canonical, canonical + 16) == 0);
        assert(munmap(page, page_size) == 0);
        std::puts("Captured Bink BZHI/CMP: tagged identity fails; canonical recursive and contended owners pass");
    } else std::puts("BMI2 unavailable: checked owner contract; captured instruction execution skipped");
#endif
}
