# Neptunia ReVerse: verified experimental game patches

These are **GoldHEN-style native memory patches** for
PPSA02721 `APP_VER=01.000.000` only. The loader validates the exact original
bytes and the title ID/version before any modification. They do **not** edit the
saved game executable or replace the emulator's independent Vulkan render scale.

User-provided SELF SHA-256:
`0f7f196413774a14048e7d121efe6b4bae1435a876005a9fbc9a46ac26bca8f2`.
The SELF container's text segment starts at physical file offset `0x3670`,
while the guest executable text RVA starts at 0. GoldHEN offsets are **guest
RVAs**, not the SELF file offsets.

## Verified targets

| Preset | RVA | Original instructions | New instructions | Reason |
| --- | --- | --- | --- | --- |
| `shadow50` | `0x3E9FB2` | `49BE000C0000000C0000` | `49BE0006000000060000` | MOVABS R14 changes the square CSM intermediate size from 3072×3072 to 1536×1536 |
| `shadow25` | `0x3E9FB2` | `49BE000C0000000C0000` | `49BE0003000000030000` | Same dimensions to 768×768; lower quality |
| `nodof` | `0x41DA50` | `554889E5535089FBE85359FDFF` | `554889E5535031DBE85359FDFF` | Changes MOV EBX,EDI to XOR EBX,EBX in confirmed DoF enable setter; keeps the function prologue and its call intact |

At `0x3E9F50`, the initialization path sets
`R14=0x00000C0000000C00`; this value flows to the
`shadowmap_temporary`, `csm_2d_array_texture` and
`csm_2d_array_texture_0`, `_1`, `_2` setup routines, including calls
to `0x22E130` and `0x23EF30`. This proves the 3072² dimension is used for
cascaded-shadow intermediate resources. No main display dimensions were changed.

For DoF, the string `Dof` is looked up near RVA `0x1C32EB` and sent to
the state setter `0x7CE40`. That setter ORs flag `0x04` into the per-scene
state. The draw callback `0x79C15` reads that bit and passes it to
`0x41DA50`, which stores the enable byte into `+0xDF6`.
Render work near `0x3F0422` and `0x3F5499` checks exactly this byte
and conditionally executes graphics calls. Clearing it in the setter is a
reversible way to disable these DoF-dependent paths, rather than editing
shader names.

## Safety and combinations

The two shadow resolutions write the **same** instructions; the Android
frontend enforces exclusive selection. They can be combined with DoF OFF:
`PresetGamePatchCombiner` constructs a single validated GoldHEN JSON for
the native loader and rejects overlapping writes, title mismatches or version
mismatches. This preserves the existing single `--game-patch` CLI contract.

No executable-patch gains are assumed without on-device timings. At 25%
shadows may visibly degrade or break cascade transitions, and DoF OFF may
change focus/cutscene appearance. Validation must include overworld, menus,
cutscenes, gameplay, all shadow cascades, save/load and performance.

Bloom, SSAO, AA, glare and other effects have not yet been proved to have
safe and effective skip points in this SELF. Do not conflate changing shader
parameter values with skipping a costly render pass.
