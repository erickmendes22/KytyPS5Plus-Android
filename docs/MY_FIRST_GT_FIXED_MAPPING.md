# My First Gran Turismo: fixed high mapping on Android

Input: `KytyPS5-Plus-diagnostics-20261024-154134.zip` and `eboot(20261024-184545).bin`.
Title PPSA24156; v01.010 is inferred from the folder name, not authenticated from metadata.

The test crashes roughly two seconds after launch, before creating shaders. The matching
SIGSEGV tombstone has about 218 MiB RSS and an access to `0xe3ffffc000`. A writable mapping
already exists at `0x63ffffc000..0x6400000000` in `KytyDirectMemory`, backing offset 0x200000.
This is a startup address-translation failure, distinct from Silent Hill's memory pressure.

The supplied eboot is a readable SELF, 109,163,961 bytes, entry 0xfef5b0, SHA-256:
`7e03d8f703f1cd7dc2cc9e6c55318450a6701725a6f66a183a6489e519bb7118`.
RVA 0xc9bcfc loads the high fixed address; 0xc9bd26 calls direct mapping, followed by
memset at 0xc9bd35. Other references are at 0x14f447 and 0x14f941.

The kernel folds the mapping to a host-compatible address, but FEX address folding was
only activated by the separate large-reservation path. A direct fixed mapping bypasses
that path, so the subsequent guest store still accesses the unfolded address.

After a successful MapDirectMemory publication in the fold domain, the ARM64 path now
activates the same AddressFold/FEX policy and existing JIT invalidation used by large
reservations. It keeps the guest return address unchanged. Failed mappings and low
addresses do not activate folding. No eboot patch is applied.

AddressFoldTests includes the exact address and passes locally. An ARM64 regression in
VirtualMemoryAllocationTests exercises rejected and successful fixed-high mappings and
the writable folded backing; it has not been executed on ARM/FEX here. Host CI compiles
the emulator and runs virtual-memory/address-fold tests; Android CI recompiles the actual
ARM64 core. Device testing must establish whether the game passes this startup point and
what happens next; this change does not establish menu or race compatibility.

The attached recent-hle ring belongs to the earlier Silent Hill session. This startup did
not reach the AGC hook that recreates it, so it must not be used as this game's last calls.
