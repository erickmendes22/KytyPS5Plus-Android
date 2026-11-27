# #759 — same-dispatch dark/control pair


## #758 real / #759 paired V4 contract (27/11/2026)

- ZIP 20261127-163934, SHA256 8edd69c67c099d3e3c75189ba270c893b7ffc34b828178a86705360db56a2fec,
  revision 3a2da6e, frame451, submit13436, P8_USE_93, tile16694, WG17.
  Slot2 publication 86016 bytes identical to stable guest backing;
  24/24 admitted dynamic fields and four category decisions match in lane2.
  Prior mask, load and accumulation checks remain scoped to the captured wave.
- Real defect in witness selection: the largest packed material RGB is a lit
  control, not the dark target. Material population 4408; 8/4408 have HDR
  luminance >=0.006 before q8; 12/4408 after q8. Contrast is already present:
  lane0 (752,664) L before=0, after=0.00000402110815;
  lane2 (754,664) L before=0.0746182434, after=0.0749275818.
  Same stencil 86; 99.58715% of final control luminance is incoming.
  The q0-after snapshot and q8-before payload are byte-identical; tile16694
  is not in q0, only q8 and q33. The earlier HDR writer is not proved.
- Offline original ISA conditional replay of #758 lane2 checks ten dynamic
  image reads (all coords/descriptors/samplers match exactly), final RGB
  bitwise, accumulated RGB <=2 ULP, and four per-light RGB terms <=4 ULP.
  #757 selected pixel (882,669) also replays with <=2 ULP
  (use actual launch lane in the archive). Reference inputs are observed
  samples and verified wave reductions, not independently sampled textures.
  Dark lane0 replay checks the first five loads and reaches missing sample
  PC0x1e8. It cannot complete from #758. Mutation of 0x1e8 coordinate,
  missing sample, and altered light2 term distinguish candidate divergence
  from incomplete, without using observed output as reference.
- #759: same q8 dispatch/workgroup; choose two admitted material lanes of
  same stencil with dark incoming HDR and strongest contrasting control.
  Tested selector on real #758 yields exactly (752,664)/(754,664).
  No new dispatch, synthetic light, or global P8 certificate. Schema3 shares
  64-lane fixed/mask fields and stores separate term, sample and light-record
  tails (3,631,296 bytes). Both paths use the same original guest binary
  and native operand versions. Correct the subnormal half comparator to
  accept adjacent rounding while treating non-adjacent zero flush as
  ambiguous. Actual Android result pending.
- Next: paired local divergence -> isolate and correct that operation;
  conditional arithmetic MATCH -> trace exact differing sample source or
  upstream incoming HDR writer, not repeat accumulator/publication tests.
  The contrast thresholds are descriptive, not expected guest brightness.
  If no absolute lit control exists, the relative-control flag explicitly
  says so; do not call it a healthy reference. P8 global still historically
  BLOCKED [0,16], no fictitious global cut.


## Reproduce
```
python3 docs/android/tools/v4_q8_pair.py DIAGNOSTICS.zip /path/to/kyty_p8_guest_ref
```
The pair auditor validates distinct lane identities, same tile and stencil,
both complete per-lane traces, actual before/after HDR, mask and record
publication, per-light terms and original-ISA conditional replay. The
scoped reference substitutes only observed image results after checking
their actual PC, light index, descriptors, samplers and coordinates. The
wave OR inputs are used only after independent comparison in the existing
contract. Numerical sample-coordinate and RGB differences are reported in
float32 ULPs (8-ULP calibration margin); a larger difference is a candidate
for instruction-level reduction, not itself a driver fix. Independent
sampling of unsupported texture/shadow formats and the producer of the
incoming HDR are not certified by this instrument.

Host validation: selector compiled with -Wall -Wextra -Werror and exercised
on #758 real bytes plus same-stencil/missing-control/out-of-range controls.
Original guest runner from Actions 46012579033 executed on real #757/#758.
Actual #758 lane2 term2 maximum 4 ULP. Missing dark lane sample stops at
0x1e8, not fabricated. Mutations on the actual control inputs rejected:
0x1e8 coordinate changed by 65536 words, missing 0x1e8, altered term2
(65532 ULP). CI validates schema/selection and compiles the native APK.
The shader instrumentation is diagnostic mode F only, leaves guest values
and draw counts unchanged. Full paired Android capture still required.
