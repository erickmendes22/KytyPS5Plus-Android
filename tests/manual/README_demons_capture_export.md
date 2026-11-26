# Demon's Souls F: file transport regression

`DsLightingCaptureExport` is the production Android selector used by
`Diagnostics.exportDsLightingCapture`. Its required set is built from the
foreground manifest, all current prelight records, the executed depth clear
before the first material loss, exact native module filenames, guest shader
sources and the transitive file references of those records. The old optional
`take(48)`, `take(96)` and pipeline limits do not apply to this required set.

The inventory distinguishes missing, stale, invalid, changed and over-budget
files. `required_complete` certifies only the transport of this file set. It
cannot certify G0, P8, shader semantics, complete operands or gameplay.

## Local run

Provide the JSON-java dependency already used by the app's JVM tests
(`org.json:json:20180813`), as a jar or a checkout at tag `20180813`:

```bash
python3 tests/manual/test_ds_lighting_capture_export.py --json-jar /path/to/json-20180813.jar
```

Run the same selector over extracted historical captures:

```bash
python3 tests/manual/test_ds_lighting_capture_export.py \
  --json-sources /path/to/JSON-java-20180813 \
  --capture /path/to/744/ds-lighting-capture \
  --capture /path/to/741/ds-lighting-capture
```

The Java fixture class is also invoked by `DsLightingCaptureExportTest` in the
app's ordinary JVM test target. It checks required files beyond both previous
count limits, distinct original/bound modules, exact UInt64 guest hashes,
recursive buffer slices, executed clear selection, stale data, path escape,
missing files, digest/length mismatches and bounded mandatory-first selection.
The budget fixture contains 40 MiB representing existing queue evidence,
25 MiB of new snapshots and 32 MiB of operands/vertex data; all required bytes
fit before optional files consume the remaining 160 MiB total/128 MiB queue
budgets.

Historical assertions preserve the #741 cycle 40367→40979 and #744 cycle
39624→40091. #744 must report the six known native SPIR-V omissions and the
missing `vs_557d65da5b786842` guest source. Neither historical capture can be
reported as complete. The local script does not download dependencies, alter
evidence, launch gameplay, build an APK or assert a rendering-oracle result.
