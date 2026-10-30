# Neptunia ReVerse (PPSA02721) — eboot / CapyPS5 compatibility audit

Date: 2026-10-30. Experimental branch: `experiment/neptunia-capy-compat`.
Source build: `experiment/capy-global-render-scale`, Android APK/CI run
[41073621662](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/41073621662) passed.

## Uploaded eboot.bin: verified signatures, NOT verified effect toggles

- User-provided PS5 SELF size: 10,423,548 bytes; SHA-256
  `0f7f196413774a14048e7d121efe6b4bae1435a876005a9fbc9a46ac26bca8f2`.
- SELF magic `4f153d1d`, embedded ELF at file offset `0x1a0`. Executable
  strings identify Neptunia ReVerse / NepPS5_EN, but a SELF alone cannot
  prove the installed update version; check game metadata separately.
- All eight original instruction signatures of the historical **v01.000.000**
  resolution-only game patch were found, with one common **file displacement
  `+0x3670`** from that historical patch table. File offsets, **not** mapped
  runtime addresses:

| Historical entry | Offset in supplied SELF |
| --- | --- |
| 0x27d7e0 | 0x280e50 |
| 0x27d7f0 | 0x280e60 |
| 0x3f20e8 | 0x3f5758 |
| 0x4294cf | 0x42cb3f |
| 0x429532 | 0x42cba2 |
| 0x3f3775 | 0x3f6de5 |
| 0x3f3916 | 0x3f6f86 |
| 0x3f39d9 | 0x3f7049 |

**Never add the file displacement to a runtime `--game-patch` offset.**
These eight historical executable-resolution patches are retired in favor of
the existing Vulkan host `--render-scale`, including 25% and 33%.

The SELF also contains the shader/resource names `ShadowMap`,
`ShadowMapList_2`, `ShadowOff`, `DrawShadow`,
`LightProjection`, `LightViewProjection`, `BloomP1`, `BloomP2`,
`hdr_ps_dof`, `ScrEff_RadialBlur`, `ScrEff_AntiAlias` and various
`fog` and `non-fog` shaders. These are **resource names, not independently
verified boolean settings or executable patch points**. No speculative
shadow/bloom/DOF OFF patch is shipped. Existing host-side optional shadow
atlas 50% scale is safer to trial per game, with a native option for rollback.

## Tested against CapyPS5 differences

Backports in this branch (source commits in
[Ghael-V/CapyPS5](https://github.com/Ghael-V/CapyPS5)):

- `e8dfafb42b`: do not reject a Vulkan device just for missing
  `samplerAnisotropy`; disable anisotropy in the created sampler. Relevant
  for PanVK/Mali (not a claim the rest of the device features are supported).
- `2b1c496d08`: preserve color image formats on 1D/3D depth-compare sampled
  textures instead of requiring unsupported Vulkan depth images. This avoids
  Adreno texture creation failures.
- `aecd9f53cb`: D16 guest depth copies between native 2-byte D16 and 4-byte
  D24/D32 host transfers, even when both sides are depth images; preserve
  original pitch, layers and depth aspect. Addresses a documented depth-copy
  crash path.
- `709aa2c7db`: decode `S_DCACHE_INV` in PS5 SMEM as a zero-operand
  no-op, instead of failing shader translation. Includes decode + SPIR-V test.
- `ffa9c62` from KytyPS5 PR #857: clamp inverted sampler min/max LOD.

Important Neptunia compatibility background:
[KytyPS5 issue #317](https://github.com/KytyPS5/KytyPS5/issues/317)
and [PR #857](https://github.com/KytyPS5/KytyPS5/pull/857).
Many game fixes already exist in this fork (alpha blending mapping, sampler
LOD bias clamp, 565 format conversion); don't duplicate.

## Deferred / requires proof

- Explicit eboot shadow, bloom, anti-alias and DoF disable patches: **no**
  verified edit instruction from supplied SELF yet.
- CapyPS5 `IMAGE_GATHER4` variants: wider shader and IR interface changes,
  needs independent regression testing against existing PS5 textures.
- CapyPS5 unnormalized sampler emulation, async CP semaphores and queue
  retry: invasive; need renderer/shader tests and gameplay comparisons.
- CapyPS5 frame generation interpolates host presents rather than increasing
  actual PS5 guest FPS; not part of a title-specific performance fix.
- Remaining GT7 features (128-byte DCC, memory pool backing) can be
  evaluated separately; do not import GT changes into a Neptunia fix branch.

## Validation criteria

1. CI must compile APK and pass native regressions. The no-op SMEM test
   validates decoded instruction boundaries, no phantom operands and valid
   resulting compute SPIR-V.
2. Verify a title's eboot and param metadata are from the same version.
3. Compare Neptunia native render scale vs 50%, 33%, 25%, with stable driver
   and temperature. Shadows at native versus 50%; compare menus, gameplay,
   alpha blending, cutscenes, audio and save/load.
4. Confirm no Vulkan depth-format or sampler validation errors. Check that
   compatibility fixes don't change previously working Astro Bot or GT7.
5. Only describe FPS gains after on-device frame-time measurement.
