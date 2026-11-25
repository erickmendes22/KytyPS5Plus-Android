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
