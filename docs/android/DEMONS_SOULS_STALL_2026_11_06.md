# PPSA01341: post-cutscene stall, device log 08:35:32

Device tested commit: `23a89a174cb6cf9b6c63e9775730bf225a4f0952`.
EBOOT SHA-256: `d499226cdc4b074eb8ec72ac03ad469bb80f48e580164c08157e6fd0c608c2c5`.

The device stops submitting guest work after suspend point 741. Repeated queue
snapshots show zero pending submissions, host commands and flip requests. This
does not establish a presentation or GPU memory budget problem.

Ten exact JIT samples of worker tid 27749 land in guest offsets 0x83b6e0 through
0x83d69b. EBOOT disassembly identifies the enclosing function 0x83b640 as the
pre-poll scheduler wait: it scans five per-worker queues at stride 0x9220 and the
worker slot at scheduler+0xe90+index*8, then invokes a watchdog after two counter
seconds. This is before the already patched empty-poll backoff at 0x83b59b.
Its repeated empty-queue observations do not justify forcing the poll to succeed
or adding a sleep as a fix for the missing task.

The `deadbeef00000007` value in samples at 0x83c608/0x83d69b is read from the
stack guard and checked on return; it is not evidence of an invalid task pointer.
R11=0x90083ee30 is a register value, not a sampled instruction pointer.

Worker tid 27756 remains inside an indefinite condition wait from guest
0x1e6ed5b. EBOOT 0x1e6ed20 implements a semaphore using a mutex, a count at
object+0x10 and a condition at object+8. The caller passes timeout -1, and the
loop waits while the count is zero. The existing samples exclude active HLE
waits, so they cannot identify the task that owns this semaphore wait.

The follow-up diagnostics sample both stale guest execution and stale HLE
waits, excluding the old non-unwound pthread_exit record. The signal handler
still only publishes register state. On the reporting thread, a guarded,
bounded RBP walk prints up to 24 return addresses as DS_STACK. Non-JIT state
retains exact_jit=0; the frame walk reads live memory and is labelled accordingly.
No event, queue, semaphore or guest scheduling decision is changed.

Host regressions cover blocked HLE selection, exclusion of pthread_exit,
frame-chain traversal, invalid memory, cycles, bounds and coherent publication.
This is diagnostic coverage, not a claim that the game stall is fixed.

Wolverine's accompanying log reaches CS77 and reports the real translated
MONITORX/MWAITX self-test PASS. It has no logged decode fault or assertion and
exits with status 0. Those observations alone do not establish visible gameplay.
The user confirms a black screen. Wolverine now enables the same observation
path through KYTY_GUEST_FRAME_TRACE=1. The existing DS-prefixed record format is
retained for parser compatibility; the launch header identifies the title.
The generic trace does not enable any Demon-specific execution policy. Launch
tests check both the positive title match and isolation of the clock/frame flags.

## Wolverine: independent GPU clock correction

The device log emits the old S_MEMREALTIME constant-placeholder warning before
CS41. PC commit [77a8f307](https://github.com/KytyPS5/KytyPS5/commit/77a8f307585933b944ffb67d24af8fb730130389)
implements this instruction with the Vulkan device clock. Port the IR opcode,
OpReadClockKHR Device emission and PC clock-shift policy to the Android fork's
ShaderDeviceFeatures interface. Reuse one scalar result across emulated wave64
halves. Query and enable VK_KHR_shader_clock/shaderDeviceClock, and include the
period in the shader disk-cache options signature.

The Android port preserves its previous UINT64_MAX fallback on unsupported
devices instead of adding an unsupported capability or a new abort. It logs
SHADER_CLOCK support, period and shift even with silent printf settings. The
new path is verified through captured instruction translation, SPIR-V validation,
multiple read counts, wave32/wave64 and clock periods including Adreno's 19.2 MHz
counter. None of those host checks establishes that Wolverine's black screen is
caused by this instruction or is fixed on the device. Demon’s Souls still needs
the blocked-task capture described above.
