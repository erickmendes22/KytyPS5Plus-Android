# Bink pthread identity collision on Android

Device logs: `KytyPS5-Plus-diagnostics-20261106-102148(1).zip` (Demon's Souls)
and `KytyPS5-Plus-diagnostics-20261106-102718(1).zip` (Wolverine), core 63508800.

Both confirm shaderDeviceClock=1, period_ps=52083, shift=0. This confirms the
previous GPU clock path was selected, not that it resolved either game.

## Binary and runtime evidence

- Wolverine SDLThread remains in Bink's recursive mutex path: HLE wait caller
  0xe1a519, stack 0xe1a603 → 0xdfe38a → 0x3e09c6b. Saved R15, the current
  pthread identity, is **0xb400007acca0ae80**. Render Thread polls for main-thread
  work at 0x3f37960/0x3ccaa20; host submission/flip queues are empty at frame 780.
- At 0xe1a59f, PLT 0x5f1f920 resolves JMPREL to `aI+OeCz8xrQ#v#G`, registered
  as PthreadSelf. Guest 0xe1a5e2 executes `mov al,63; bzhi rax,rsi,rax`, then
  compares the masked owner to the unmasked identity in R14. On a match it
  increments recursion; otherwise it waits indefinitely in 0xe1a380.
- Demon's Souls uses the same owner protocol at 0x1e6ec32/0x1e6ec39, entering
  the wait path at 0x1e6ec4c on mismatch. All four BinkAsy threads have that wait
  in their captured stacks. BPE worker waits for Bink async results through
  0x9826a7 → 0x1e54c80 → 0x1e54d89 → 0x1e54230 → 0x1e6ed20.
- Native PthreadSelf returns g_pthread_self, a PthreadPrivate allocated with
  ordinary `new`. Android supplies these allocations with top-byte tag 0xb4.
  The tag already sets bit 63, which Bink reserves for contention.

Therefore a recursive acquisition by the same thread compares
`0x3400007acca0ae80 != 0xb400007acca0ae80` and enters the other-owner wait.
This is an ABI defect, not justification to signal a guest condition artificially.

## Correction and checks

Use Android's documented process-start compatibility switch
`android:allowNativeHeapPointerTagging="false"`. New host objects exposed as
guest handles then have untagged identities. Do not truncate tagged pointers at
HLE return or free: all native allocations must retain the allocator's own
pointer representation throughout their lifetime.

References:
- [AOSP tagged pointers](https://source.android.com/docs/security/test/tagged-pointers)
- [Bionic tag implementation, including top-bit rationale](https://android.googlesource.com/platform/bionic/+/1b2e844745e4541284f092017ed887147d42b445/libc/bionic/malloc_tagged_pointers.h)

The host regression executes the exact captured BZHI opcode: tagged self/owner
fails, canonical self/owner succeeds, canonical contended owner succeeds, and a
different owner fails. The package audit parses binary AndroidManifest.xml and
requires the application attribute to be boolean false. The previous APK fails
this audit as expected. The new package must pass it before delivery.

No native source or build input changes are required. Pin native reuse to the
audited 63508800 artifact 13503539630, digest
7ec3663a968a78c3e6bf61c007a802d76cc87c3d9e7be572fa2aae250e5adc7c.
The reuse script still verifies source/build equivalence and every library's
bytes; otherwise it builds normally. On-device playback/transition remains the
acceptance check for the game stalls.
