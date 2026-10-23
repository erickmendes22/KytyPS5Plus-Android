# KytyPS5 Android: roadmap / what's next

_Last updated: 2026-10-23._ Items are ordered by expected gain per effort. Anyone can pick one up; open an issue
saying which so work isn't duplicated.

## In progress

1. **Record-thread / GPU front-end speed-up.** This is the main limit for Astro Bot and Delivery Service now.
   - **Candidates:**
     - place the record, front and walk threads on the prime cores (cpu6–7 sit mostly idle while the hot
       threads run on cpu0–5 at ~1.7 GHz);
     - drop or defer front→record drains for packets that don't need an idle record thread (~128 drains/frame
       on Astro: POP_MARKER, EVENT_WRITE, RELEASE_MEM, ACQUIRE_MEM);
     - cheaper per-draw descriptor and state work.
   - **Target:** Astro 20 → 25 fps, Delivery Service 35 → 45 fps.

## Next

2. **Performance mode toggle in the in-game menu.** Switch presets without going back to the launcher.
   Small and self-contained (Kotlin, `android/app/.../presets`).
3. **Zero-copy guest memory → Vulkan (Commandos / UE5).** UE5 spends its frame copying large guest buffers.
   - `VK_EXT_external_memory_host` is not in Turnip, and soft-dirty is not in the 6.6 kernel, so this needs
     either a Turnip patch or an import path via AHardwareBuffer/dma-buf.
   - Estimate 1.5–2 weeks. Without it UE5 games stay unplayable.
4. **Upstream round 2.** These upstream KytyPS5 commits conflict with our front-end, caches and shader changes and need porting:
   - **a358b847:** the flip-wait stall moves into the command processor.
   - **b5405794 and 4059179e:** colour-clear selection on the GPU and proven compute fills, which cut GPU→CPU readbacks.
   - **More shader work:** the remaining DPP/ALU instruction cuts and the DPP crash fix 9dcff95e.
   - **Later:** the bindless image-heap series and the shader "Profile" refactor.
   - **Note:** upstream 66f488a1 makes boolean options value-less, so the app's launch arguments must change with it.
5. **More games.** Each new title shows which fixes generalise. Please report the title ID, how far it gets,
   fps, and a log (`--printf-direction File`, `files/_kyty.txt`).
6. **Resolution patches for more titles.** This follows the Astro `perf720` method: find the render-size tables
   in rodata, ship a `--game-patch` JSON in a preset. See `android/app/src/main/assets/presets/PPSA21567/perf720/`.

## Known problems

- Astro Bot: blocky edges in Performance mode (half-rate RT edges); a sky stripe with the `kyty-a830 v4` driver
  (use mainline v32).
- Delivery Service: shadows look like simple blobs. That seems to be the game's own low-spec choice; an HLE trace
  showed no under-reporting.
- The tablet throttles after a few minutes of heavy load. Any A/B must compare runs at the same GPU clock step
  (the `[perf-probe]` line).

## Tried and ruled out (don't redo without new evidence)

- **Submit and barriers:** submit coalescing (Turnip v32 `vkQueueSubmit` blocks on the GPU), barrier narrowing,
  label forwarding.
- **Guest memory tracking:** fault windows > 64 KiB, ordered drains, "hot page" tracking (−17%), soft-dirty page
  tracking (not in the kernel).
- **Drivers and scaling:** the `kyty-a830 v4` driver (slower), `--render-scale` for UE5 (UE renders its own size),
  reporting 1080p VideoOut to UE.
- **Memo:** the SRT memo while walk fan-out is on (−27% on Astro, hence `auto`).
- **Dead ends:** BVH emulation variants; skipping RT entirely (+1.9 fps at most).
