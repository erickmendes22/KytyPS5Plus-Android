# P8 native replay candidate

The #746 journal closes at frame 222, whereas the actual compositor's dark
scene input at frame 481 is CompositeAuxAddress, not the transparent overlay.
The candidate captures the known four material layers and actual depth
before each known deferred-light consumer (0, 8, 33), HDR after those
dispatches, complete compositor output, and the flip resource identity.
It includes a bounded GFXReconstruct trace of the same translated Vulkan
commands, built from upstream commit 4401228c791b11fb7ea15b93795cbd1e938307c0.
Markers associate raw observations with replay transfer commands; the
exporter records complete lengths and SHA-256, or rejects the whole oversized
capture. Readbacks/trim can perturb timing. The capture layer must be
available with the original driver; failure is explicit.

Offline (no second device requested):
1. python3 docs/android/tools/p8_native_replay.py extract DIAGNOSTICS.zip /tmp/p8
2. Convert the emitted gfxr file with pinned gfxrecon-convert --format jsonl.
3. python3 docs/android/tools/p8_native_replay.py plan /tmp/p8 TRACE.jsonl /tmp/plan.json
4. gfxrecon-replay --dump-resources /tmp/plan.json --dump-resources-dir /tmp/replay TRACE.gfxr
5. python3 docs/android/tools/p8_native_replay.py compare /tmp/p8 /tmp/plan.json.mapping.json REPLAY_rd.json /tmp/replay

Both candidate differentials (boundary and post-boundary) are executable.
This is not an independent guest implementation: translated shaders,
commands, checkpoint memory and state may have a common-mode defect. An
identical dark replay proves neither P8 PASS nor FAIL; a differing boundary
does not by itself prove causality for the final symptom. Unknown consumers,
unsupported replay features, missing marks, size or identity mismatches
must not be treated as successful decisions. No global class is pruned
by this candidate. Do not describe the candidate as a completed P8 oracle.
