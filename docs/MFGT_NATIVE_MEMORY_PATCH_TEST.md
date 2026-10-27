# My First Gran Turismo (PPSA24156) — integrated memory test

This document describes experiments shipped **on the same branch as the GT memory collector**. It does not claim that any test has fixed Android OOM.

## Executable identity and tested sites

- Supplied file: `eboot(20261027-135440).bin`.
- Format: PS5 SELF with an embedded x86-64 ELF at raw offset `0x1a0`.
- SHA-256: `7e03d8f703f1cd7dc2cc9e6c55318450a6701725a6f66a183a6489e519bb7118`.
- Text segment: RVA `0x0`, raw-file offset `0x4000`, length `0x3e4548c`.
- Application label in diagnostics: My First Gran Turismo `PPSA24156`, v01.010.
- Native preset patch declares `APP_VER=01.010.000`; the loader checks this against guest metadata and **refuses to patch if different**.
- All eight original instruction sequences below were verified at their mapped ELF RVAs and the replacements preserve instruction lengths and opcodes. Four 10-byte RIP-relative stores are unique in the submitted SELF.
- **Only the immediates carrying native width or height values are changed**; the two resolution branches converge on the same `0x2f47b0` setup routine. The bytes are applied to executable memory, not the original game file.

| ELF RVA | Native behavior | Test behavior |
| --- | --- | --- |
| `0x2f1e79` | store height 2160 | store height 720 |
| `0x2f1e83` | store width 3840 | store width 1280 |
| `0x2f1e8d` | pass width 3840 | pass width 1280 |
| `0x2f1e92` | pass height 2160 | pass height 720 |
| `0x2f1ee8` | store width 1920 | store width 1280 |
| `0x2f1ef2` | store height 1080 | store height 720 |
| `0x2f1efc` | pass width 1920 | pass width 1280 |
| `0x2f1f01` | pass height 1080 | pass height 720 |

This changes **game-selected video/output surface setup**. It has **not** been proved to change every internal scene texture or reduce the 2–3 GiB of sampled image allocations seen in logs. As with Silent Hill, the binary-level identity and byte tests are separate from actual device-performance evidence.

## Selectable Android test presets

- **native720:** opt-in executable patch from `assets/presets/PPSA24156/native720/launch/gt_native720.json`.
- **mem75 / mem50:** host render-target scale 75% / 50%, respectively; these are *not* native patches and cannot be advertised as such.
- **native1080:** optional title-scoped kernel operation submode test; does not rewrite the executable.

The same APK retains the experimental title-scoped GPU-written-image collector and the 32 MiB VMA block policy. Keep normal title- or app-level resolution and the shader/Turnip investigation separate.

For clean comparisons, test **one native mode at a time**. In particular, do not combine native720 and native1080 on the same launch. Record the game-reported native image sizes, GPU `device_usage`, `vma_allocs`, `images`, `host_available`, peak PSS, and process exit reason. Output resolution and device RAM are not guaranteed to improve simply because a patch is accepted.

## Rollback / guards

All presets are opt-in; disabling native720 restores the unmodified guest executable on the next launch. The loader checks title ID, declared game version, executable name, and every site's exact original bytes. A mismatch causes no native-720 instruction writes. No file in the user's game directory is overwritten.
