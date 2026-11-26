# PPSA01341 01.007.000 — causal matrix
References: Jetsku dd9f990 (#130 documents fog-transition crash), Chenxiao07
66cdb39 (v20261016 family reports gameplay, no identical GPU capture).

## Evidence ledger (9 ZIPs through 201032)
Unique captures: cb3b arithmetic 138/138, f0c140 count transfer 1344/1344,
fb18 first gate metadata 1748/1748 matched. fb18 84/107 nonempty inputs
have zero outputs, also in bright frames; not proven wrong. Two small
vertex/depth/material pairs project consistently. Dominant depth draw
executes after rejection, not the initial cause. Sampled atlas coefficient
transfer and descriptor 224 bytes match. Radiance provenance and first
loss of albedo remain open. Do not reopen excluded operations without
new evidence.

## Modes, same save / camera / resolution / driver
A control (no full dumps); B1 only index culling; B2 only sun shadow
index culling; reversible managed args, no eboot changes.
C1 PC-style coherent CPU reads of indirect count/records/pending
NUM_INSTANCES, log dirty/pending/changed/fallback; C2 attachment-write
availability barrier in GPU indirect fixup. D1 strict cross-frame order
without ahead-of-GPU semaphore; D2 finish before binding selected compute
dispatches (SRT specialization may already have run; not all queues).
E/F heavy capture: E independent CPU oracle for cb3b arithmetic; F
geometry/G-buffer/relight plus wider fb18 capture and same oracle.
Full isolated GPU replay not yet implemented; spatial/BDA operands and
unmatched variants remain explicitly unavailable. Fix unreachable fb18
probe guard; exact fingerprint validation required; export raw modules
for offline analysis. A–D visual result + session log; E/F full ZIP.
No lighting correction or gameplay verification claimed.

## Exit criteria
First mismatching producer/consumer or guest-semantic output. If an
isolation mode recovers the image, narrow to specific operation before
making default change. If negative, use E/F data to discriminate M1–M7.

## Capture completeness
Both native fb18 fingerprints 737447ebe010052e and bc2046dc2a822ad8
have identical defining instructions at all 157 recorded sites, verified
from 185122. Accept only those exact hashes and validate the instrumented
module; export unknown modules without instrumenting. Compare each captured
first-gate input from actual GPU descriptors to the guest ISA-derived gate
and the recorded lane's count, active state, object ID, flags and pass.
Missing values are unavailable, not a pass. This narrows M1 but is not a
full offline visibility replay. A–D exclude cached full diagnostic dumps.

## Build #717 / 53d8085 — mode F, ZIP 20261125-210141
Session 21:00:05 GMT-3, PPSA01341, Turnip GameHub 26.3 DD GMEM,
720p output. Capture enabled; fb18 fingerprint 737447ebe010052e assembled
and validated. Eight dispatches, 136 active object lanes: first-gate replay
136/136, BDA sampled load vs backing 272/272 (not proof of all BDA),
float comparisons 272/272, subgroup-any decisions 64/64. In all 136 lanes
site 3979 = 1.0 and the actual unordered less-than 1.0 at site 3980 is
false; site 4097 = 0 and no subsequent atomic emission reached. Same
condition in lit-reference and dark windows; no count-table changes within
any of eight observed fb18 dispatches. Do not call this a causal culling
bug before establishing the producer of the value at 3979.

M3/M6/M7 concrete inconsistency: cb3b9c1ea64fe876, frame 307 submit 9276
capture_seq 20, output guest 0x20817daa0. Descriptor slot 0 address
0x20934ae80 index 12 GPU=0 backing=0, slot 1 address 0x2093622b0 index 85
GPU=0; secondary limit 84, primary limit 30. AMDGPU ISA 0x44..0x50 and
0x70..0x90 predict [1,1,1,0,0], observed [1,1,1,0,3]. Slot 2 before GPU
[1,1,1,0,1], before CPU backing [1,1,1,0,3]; both input ranges GPU-dirty.
7/8 captured cb3b operations match. Source writer candidates 9ee22f0ba0ad42f8
and c4e786c3a7c0eadf, same frame/submit; range overlap is not proof of last
writer. Both work counts zero; not proof of scene-loss causation and occurs
after onset frame 284. Need distinguish captured vs actual shader input,
output publication and synchronization, not force DWORD 4.

Bridge (frame, G-buffer colored/64, HDR lit/64, mean): 164 64 64 .07518336;
284 0 0 0; 332 12 0 .00007316; 476 3 0 .00000971. M4/M5 remain open.
Next discriminatory existing APK mode D2 (same scene, no heavy captures):
checks selected compute completion before bindings; negative visual result
cannot rule out all coherence errors or certify cb3b replay. B1 only if D2
negative: independent original index-culling path, not fb18 object gate.
No new APK necessary for these modes, no rendering fix claimed.
