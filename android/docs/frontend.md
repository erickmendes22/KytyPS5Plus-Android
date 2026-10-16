# Android launcher: frontend features and provenance

KytyPS5 is GPL-2.0-only, so only GPL-2.0(-or-later), LGPL-2.1 and MIT/BSD code may be copied in.
The launcher has no AndroidX/Compose dependency; ideas and code from the projects below were ported
to platform views. Files that carry ported code name their source in a header comment.

| Feature | Files | Reused from (license) |
|---|---|---|
| GPU driver browser | `drivers/` (`GitHubReleases`, `DriverCatalog`, `GitHubReleaseSource`, `DriverAdvisor`, `DriverDownloader`, `DriverBrowserDialog`) | Flow of RPCSX-UI-Android `ui/drivers/GpuDriversScreen.kt` + `utils/DriversFetcher.kt` (GPL-2.0); recommendations from research/frontends.md |
| Touch controller | `input/` (`PadState`, `PadGeometry`, `OverlayLayout`, `OverlayProfileStore`, `OverlayGeometry`, `GamepadBridge`, `TouchControllerView`, `ControllerEditor`), `org/libsdl/app/KytyVirtualPad.java` | D-pad thirds rule and stick circle-to-square projection from Dolphin's InputOverlay via Vita3K (GPL-2.0-or-later); normalised global/per-game layouts after Vita3K `OverlayLayout`/`OverlayStore` (GPL-2.0); editor flow after RPCSX `OverlayEditActivity` (GPL-2.0) |
| Library grid + per-game settings | `library/` (`ParamSfo`, `GameSettings`, `CoverLoader`, `GameGridAdapter`, `GameSettingsDialog`), `GameLibrary.readEntry` | Layout of Vita3K `AppsListScreen` / RPCSX `GamesScreen` (GPL-2.0); per-game env editor after Eden (GPL-3.0, design only) |
| Performance HUD | `hud/` (`HudSources`, `HudFormatter`, `PerformanceHud`) | Own implementation; GameNative's HUD (GPL-3.0) as design reference only |

## How input reaches the emulator

The overlay never calls native code of its own. `KytyVirtualPad` registers an SDL joystick through
SDL's existing Android JNI (`SDLControllerManager.nativeAddJoystick`), from SDL's own device poll so
it cannot race SDL initialisation. SDL builds a gamepad mapping from the button/axis masks, the
emulator opens it on `SDL_EVENT_GAMEPAD_ADDED` (window.cpp) and maps SDL buttons to PS buttons as
for any controller. Triggers are axes 4/5 (rest -1, pressed +1). Touch-pad click is SDL BACK, which
window.cpp turns into a centred touch-pad press for pads without a touch pad. While the overlay is
visible it consumes all touches, so the legacy "any tap = Cross" fallback (hostInput.cpp) only
applies with the overlay hidden.

## HUD data

FPS and frame time come from the emulator's window title (`frame: N, fps: X`, refreshed once per
second by window.cpp), which SDL forwards to `Activity.setTitle`; frame time is the 1 s average.
GPU load (`/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage` or `gpubusy`), GPU/CPU temperature (kgsl
`temp`, thermal zones named `gpu*`/`cpu*`) are shown only when readable; the sources found are
logged once under the `KytyHud` tag. RAM comes from ActivityManager and `/proc/self/status`,
battery from the sticky `ACTION_BATTERY_CHANGED` intent.

## Per-game environment variables

Set in the game settings dialog (one `KEY=VALUE` per line) and applied with `Os.setenv` in
`GameActivity.onCreate` before SDL loads the emulator, so Turnip reads e.g. `TU_DEBUG=sysmem` when
the Vulkan driver loads. Keys set for a previous game in the same process are unset first;
`LD_PRELOAD`, `LD_LIBRARY_PATH` and similar are refused.
