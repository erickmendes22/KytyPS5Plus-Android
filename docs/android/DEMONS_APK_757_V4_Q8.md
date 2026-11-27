# APK #757 — V4 local q8 contract

Target is the actual deferred lighting dispatch (guest shader 3dfe402653925fbf),
not a new global P8 proof. No renderer lighting fix or altered input values.
Mode F (KYTY_DS_MATRIX=8) enables the bounded native observer. It chooses one
workgroup and material-bearing lane from the actual queue-8 tile list, preferring
stencil bit64 and nonzero material. It retains the four bound GPU buffer ranges,
original descriptors, linked depth/stencil/G-buffer images, dispatch dimensions,
and physically separate before/after HDR snapshots. The binary trace is 1,664,112
bytes, and is gated by one workgroup and selected lane where appropriate.

The host comparator executes original GFX10 ISA for the tile prologue and
arithmetic operations; checks native stencil/material loads, exact buffer words,
64-lane outer/inner OR and selected light indices; checks accumulated RGB and
HDR load/add/store with explicitly scoped FP conversion tolerances. It retains
actual coordinates/descriptor/sampler/sample values for dynamic image reads,
but those samples are not independently certified without their source texels.
A match is local, not proof of correct illumination. A divergence identifies
the tested instruction and expected/observed words. Incomplete capture,
unsupported formats, iteration overflow or missing provenance are separate.
When these contracts match, the next contract is the actual consumed light
mask/record or sample input identified by the report, not arbitrary new probes.

## Verification before device acquisition

The original q8 code from #756 has SHA-256
c99fdd9616b9975702f3fec398d27d29ccde9a76b75483d1248228e1c03fcaa6.
The independent executor reproduced 64/64 coordinates of workgroup 0, including
lane15=(799,553), using reconstructed captured q8 prefix/tile data. Original
v_mac_f32 at 0xc10 was executed with a controlled operand set. The comparator
fixtures passed matching and mutated outer/inner OR, load/descriptor identities,
secondary addition, missing records, truncated capture and overflow rejection.
These are local reference/fixture tests, not an Android observation.
Android CI must compile the native code, run focused regression checks, and
produce a signed APK before device acquisition.

## Run

Use the same save and camera as #756; select mode F, retain the export once
the dark target frame completes, and run:
```
python docs/android/tools/v4_q8_contract.py <diagnostic.zip> <kyty_p8_guest_ref>
```
The independent runner from Actions 46012579033 is the reference executable.
No P8 half is marked cut. The next real capture is required to compare the
actual Android trace and decide this local contract.
