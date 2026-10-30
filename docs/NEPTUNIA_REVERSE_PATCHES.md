# Neptunia ReVerse · PPSA02721 · v01.000.000 (experimental)

Original user's PS5 SELF: `eboot(20261029-232358).bin`, SHA-256
`0f7f196413774a14048e7d121efe6b4bae1435a876005a9fbc9a46ac26bca8f2`.
The five ELF LOAD segments were reconstructed without modifying the user's file.
The following original instruction bytes were independently checked against BOTH the
unpacked ELF and the unmodified SELF's code segment:

| RVA | Original instruction | Interpretation |
| --- | --- | --- |
| 0x27d7e0 | b8 00 0f 00 00 | return screen width 3840 |
| 0x27d7f0 | b8 70 08 00 00 | return screen height 2160 |
| 0x3f20e8 | b9 00 0f 00 00 41 b8 70 08 00 00 | 3840x2160 output setup |
| 0x4294cf | ba 00 0f 00 00 b9 70 08 00 00 | 3840x2160 output setup |
| 0x429532 | be 00 0f 00 00 ba 70 08 00 00 | 3840x2160 fallback output setup |
| 0x3f3775 | 49 be 80 07 00 00 38 04 00 00 | 1920x1080 target initializer |
| 0x3f3916 | 48 bb 80 07 00 00 38 04 00 00 | 1920x1080 target initializer |
| 0x3f39d9 | 49 bf 80 07 00 00 38 04 00 00 | 1920x1080 target initializer |

These presets replace **instructions in guest memory only** using Kyty's existing
`--game-patch` subsystem. Their JSON has `off` signatures, title and APP_VER
guards; mismatching executables are NOT changed. Disabling a preset restores
normal behavior next launch. Select just **one** resolution preset.

Modes: res1080 (1920x1080), res720 (1280x720), res540 (960x540),
res360 (640x360). Performance intent comes from reducing render target dimensions
and work, NOT from skipping shader compilation, removing draws or disabling
graphics effects.

## Validation boundaries

These are **byte-verified experimental modifications**, not gameplay-verified.
The game has other hard-coded sizes in the executable, which can correspond
to UI, video, media codecs or distinct render surfaces. Without device testing
we cannot claim that every in-game render pass is scaled or that FPS improves.
In particular, not every 3840x2160 reference was changed: indiscriminately
rewriting constants would corrupt non-rendering code. Keep the game's output
resolution/host render scale stable during the first comparison, then compare
graphics, FPS and stability with each option.

Android game classification: `android:appCategory="game"`, legacy
`android:isGame="true"`, plus the `CATEGORY_GAME` launcher intent category.
Those make Kyty eligible for game discovery, but proprietary REDMAGIC Game Space
inclusion is controlled by the device and cannot be guaranteed by the APK.
