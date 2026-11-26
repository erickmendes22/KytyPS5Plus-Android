# Independent vertex-position reference

`prelight_vertex_oracle.py` executes the guest gfx1010 instructions on the
backward dependency slice of `EXP POS0`. It uses consumed scalar read regions,
native buffer payloads, descriptors, vertex indices and instance indices from
the same captured draw. Native XYZW is compared after reference evaluation;
native probe input/matrix columns never seed the reference.

```bash
python3 docs/android/tools/prelight_vertex_oracle.py diagnostics.zip --output vertex.json
python3 tests/manual/prelight_vertex_oracle_test.py
```

Python integration is `audit_capture(Path(...))`. NumPy and an LLVM shared
library with the AMDGPU gfx1010 disassembler are required. There is no network
dependency at execution time. `--llvm-library` selects a nonstandard library.

## Result scopes

| Status | Meaning |
| --- | --- |
| `POSITION_MATCH` | Captured native POS0 agrees with the independent calculation for the listed invocation identities and operands. |
| `POSITION_DIVERGENCE` | A same-operand position discrepancy exists under the declared numerical model. Inspect its magnitude and numerical contract before treating it as a renderer defect. |
| `UNSUPPORTED_DRAW` | A required instruction, operand, identity or observation could not be interpreted. Other draws retain their results. |
| `POSITION_REFERENCE_ONLY` | Guest positions were calculated from complete direct-draw operands, but no native POS0 observation exists. No observed value is fabricated. |
| `ZERO_EXECUTED_INVOCATIONS` | Ordered, same-command source/fixed indirect arguments imply no vertex work. Their upstream producer is not certified. |
| `NATIVE_POSITION_UNAVAILABLE` | Native positions and sufficient direct invocation identities are unavailable. |

`draws` contains comparisons with existing position records. `unobserved_draws`
accounts for additional retained input records, and `draw_classification`
lists their union exactly once. These are scoped results within the causal
investigation; none is an integral P8 certificate or a build permission gate.

## Extensions validated with capture #745

The first pass compared 27 of the 45 native-position records. The remaining
18 were resolved offline using the already-exported guest bytes and operands:

- The explicit SDWA compare destination in VS `a720...`, PC `0x1a0`, is S16.
  The initial reference incorrectly invalidated VCC, destroying an unrelated
  jitter dependency. Correcting the reference destination resolved that draw.
- Fifteen `557d...` material draws share a saved-EXEC/restore region whose
  conditional writes only feed parameters outside POS0. Its exact full binary
  fingerprint enables the bounded region; writes remain unknown, and any
  dependency from that region to POS0 still rejects a comparison.
- `553d...` and `ab04...` required shift/OR index assembly, float-to-unsigned
  conversion, and output multipliers. Existing UNORM buffer decoding was
  sufficient; no new capture or fitted constant was required.
- The auxiliary reference-only shader required signed integer-to-float
  conversion and reverse-operand shift. Its prefetch-size hint differs while
  the remaining NGG admission/register convention is byte-identical.

The semantics come from the primary
[AMD RDNA1 ISA](https://docs.amd.com/v/u/en-US/rdna-shader-instruction-set-architecture)
(sections 3.5, 6.2.2, 6.8, 12.8 and 12.12), with instruction decoding by
[LLVM gfx1010](https://releases.llvm.org/19.1.0/docs/AMDGPU/AMDGPUAsmGFX10.html).
The compare destination is encoded by SDWA; shift/OR masks the shift to five
bits; float-to-U32 truncates and saturates with NaN becoming zero. Output
modifiers honor the captured IEEE and output-denormal controls. Tests use
analytic answers and actual instruction-byte mutations, independently of
the captured native result.

## Numerical and capture limits

The evaluator uses binary32 round-to-nearest-even and separate MAD multiply
and add, with denormal handling stated in each report. It records the actual
FP32 mode, IEEE flag, and modifier/flush events. Other rounding modes fail
explicitly when consumed. A position mismatch is not automatically a hardware
conformance failure: approximation, rounding, and FTZ must be assessed at the
specific operation. NaN classes, NaN payloads, signed zero and exact bits are
reported separately. The default ULP threshold is zero; `--max-ulp` is an
explicit analysis threshold, never a fitted hardware guarantee.

New scalar-region captures never fall back to the legacy flattened-SRT map.
Legacy maps remain clearly labeled premises for older evidence. Only needed
scalar DWORDs are required. GPU payloads never fall back to CPU backing bytes.
Index/base/first-instance association is checked without adding offsets twice.

The reference assumes already-admitted invocations. It does not certify NGG
admission, producer versions, the entire VS/PS, expected visibility or the
final image. The optional sky raster is the existing bounded raster model
fed by independently calculated positions; its precision and edge-rule
limitations remain explicit.

For #745, all 45 observed draws matched **73,580 vertices and 294,320 XYZW
components bit for bit**, with no NaNs, no subnormal flushes and no ULP
tolerance. The other five inputs comprise three reference-only direct draws
and two zero-work indirect draws. The local position comparison found no
rendering counterexample. This result preserves the same causal case and
allows the investigation to proceed to the origin of its consumed operands.
