# Wolverine MONITORX/MWAITX on FEX

Device build 7c752a51 reaches CS55/VS6/PS9, then reports `Invalid instruction in entry block: 900041127`. Bindless validation no longer causes the startup exit.

Uploaded EBOOT SHA-256: d42bb97536fcead99cc9cb3a75fe85c660a31864e978200b517cf715d43c8b72.

SELF code segment maps physical 0x30390 to virtual zero. At virtual 0x41127 (`0x900041127` after relocation), bytes `0f 01 fa` decode as MONITORX. At 0x4113d, `0f 01 fb` is MWAITX. The surrounding routine checks ownership, performs `lock cmpxchg`, retries until a time limit and then falls back to a semaphore. Other matching wait pairs occur at 0x42887/0x42898, 0xc9197/0xc91a8, 0xc9997/0xc99ad and 0xc9b67/0xc9b79.

Kyty native x86 already handles these opcodes in TryEmulateMonitorxMwaitx with no-op/yield while preserving guest state. FEX's pinned decoder instead classifies them TYPE_PRIV and supplies no dispatcher entry. The compatibility patch marks just these two opcodes executable and maps MONITORX to no state change, MWAITX to the same Yield IR as PAUSE. Guest atomics, flags, GPRs, lock counters, retry branches and timeouts are preserved. This is a spurious-wakeup fallback, not host monitored-store wakeup emulation.

A hand-assembled guest probe checks RFLAGS and input/sentinel registers. The FEX CPU regression and Wolverine's main-entry startup execute the original opcode bytes through the translator. Android log marker: `[FEX_MONITORWAITX] PASS registers_and_flags_preserved=1`. The native host test checks the probe harness success/failure branches with NOP/PAUSE substitutions; it does not claim to execute FEX on an x86 host.

Demon's Souls is independent: latest log confirms the clock-unit correction but stops at frame 116, with zero guest GPU commands and zero flip requests. SDLThread waits on KernelWaitEventFlag. One BPE worker has no subsequent HLE calls for about 49 seconds after allocator mutex unlock at guest 0x9015df5c3. This last return is not the stalled instruction. A marked SIGURG probe samples that stale guest thread through FEX's signal-context RIP reconstruction, without releasing locks or synthesizing completion events. Samples outside JIT code are labelled non-exact and include host PC/LR/module base. No Demon gameplay fix is established by these observations.
