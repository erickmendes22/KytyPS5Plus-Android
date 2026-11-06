# Demon’s Souls PPSA01341 clock unit mismatch

EBOOT SHA-256: `d499226cdc4b074eb8ec72ac03ad469bb80f48e580164c08157e6fd0c608c2c5`.

Static evidence from the uploaded SELF (executable bytes at physical offset 0xfe50):

- Guest 0x831a40 executes `rdtsc; shl rdx,32; or rax,rdx; ret`.
- Guest 0x831a63 calls PLT 0x1e74660. JMPREL row 0xa7 resolves GOT 0x26d2a40 to NID `1j3S3n-tTW4#v#a`, registered as `KernelGetTscFrequency` in `src/libs/libKernel.cpp`.
- Guest 0x831a68 caches this frequency at guest 0x32e6538.
- Worker helper 0x83b658 reads that same frequency. At 0x83b6b3 it doubles it and at 0x83b707 reads the direct RDTSC helper. It compares the elapsed direct ticks to this frequency-based threshold at 0x83b718.
- Native Android `KernelGetTscFrequencyNative` returns CNTFRQ_EL0. The attached HLE log records 0x124f800 = 19,200,000 Hz.
- Pinned FEX 5285020de0b30dbdae7b44d0d83541af2d13d002 defaults SmallTSCScale=true. `FEXCore/Source/Interface/Core/Core.cpp` shifts a low-frequency counter until it reaches at least 1 GHz. 19.2 MHz therefore becomes 1.2288 GHz (shift 6).
- Imported KernelReadTsc/process-counter stubs already shifted translated RDTSC back down. Direct guest RDTSC bypasses those imported stubs.

Thus the worker compares elapsed ticks at 64x the units of its threshold. A nominal two-second threshold expires after 31.25 ms. This establishes a clock contract bug, not proof that it causes the post-cutscene stall.

The title-scoped correction sets FEX SmallTSCScale=0 before context initialization and makes imported clock stubs use shift zero too. Native kernel counters/frequencies stay in the same units. Only confirmed PPSA01341 metadata emits KYTY_DS_NATIVE_TSC=1. Other titles retain the existing policy.

Verification: standalone clock-unit regression reproduces the 64x mismatch and checks consistent direct/imported units at 19.2/24/100/1000 MHz. Android title-isolation regression and full native/shader build are required. Device logs should show `[DS_TSC] FEX SmallTSCScale=0` and `kernel_frequency_hz=19200000 imported_counter_shift=0`.

Wolverine’s independent regression aborts on SPIR-V topology validation in a2b84374. Shared set-1 bindless images must not be counted as stage-local set-0 descriptors. The new emission regression checks mixed native/bindless and all-bindless cases with SPIRV-Tools.
