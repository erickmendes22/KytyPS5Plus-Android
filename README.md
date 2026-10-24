# KytyPS5 Plus

[![Version](https://img.shields.io/badge/version-0.1.0-38AAEB.svg)](docs/releases/v0.1.0.md)
[![Platform](https://img.shields.io/badge/platform-Android%20ARM64-3DDC84.svg)](#requirements)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

**A PlayStation 5 emulator for Android, with FEX CPU translation and native Vulkan rendering.**

KytyPS5 Plus is the Android fork maintained by [erickmendes22](https://github.com/erickmendes22)
/ [Daedalus Emulator](https://www.youtube.com/@daedalusemulator), based on
[tarek369/KytyPS5-Android](https://github.com/tarek369/KytyPS5-Android).
It combines the KytyPS5 core with a landscape mobile interface, touch controls,
per-game settings and integrated Turnip driver management.

[Download releases](https://github.com/erickmendes22/KytyPS5Plus-Android/releases) ·
[Release notes](docs/releases/v0.1.0.md) ·
[Build artifacts](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/workflows/android.yml) ·
[Report an issue](https://github.com/erickmendes22/KytyPS5Plus-Android/issues)

## Features

- **FEXCore x86-64 translation** on Android ARM64, with native Vulkan rendering.
- **Landscape interface** with folder-based game discovery and per-game settings.
- **Astro Bot `no-rt` preset** for PPSA21564 v01.007.000, selecting the existing
  non-RT lighting path and disabling unsupported GI/RT probes.
- **Resolution and internal render scale** settings, with per-game overrides
  and an optional GPU Turbo setting.
- **Mainline Turnip V32 bundled** and selected by default on supported Adreno 8xx
  devices. Driver ZIP import, online downloads and per-game driver selection are available.
- **Customizable touch controls** with editable layouts, plus physical controller support.
- **Full, compact and minimal performance HUD** modes.
- **Scrollable in-game menu**, opened with the menu button or a leftward swipe
  from the right edge.
- **Diagnostic ZIP export**, including session logs and Android process-exit reports.
- **Storage access requested on first opening only**, with manual permission access
  available afterward.

## Requirements

- Android 12 or newer (API 31+).
- An ARM64 device with a compatible Vulkan GPU. The bundled Turnip default targets
  Qualcomm Adreno 8xx; compatibility with other GPUs is not established by this release.
- Your own game files. Memory requirements depend on the title.

## Getting started

1. Install **KytyPS5-Plus-0.1.0.apk** from this fork's releases or build artifacts.
2. On first opening, allow **All files access** when prompted. If you decline,
   you can grant it later using the permission button under the library options.
3. Tap **Choose game folder** at the top of the library actions and select the
   folder containing your game or game folders.
4. Open a game's settings to adjust resolution, render scale, driver or presets,
   then select the game to launch it.
5. During gameplay, use the menu button or swipe left from the right edge to
   access the overlay and HUD settings or exit the game.

For Astro Bot PPSA21564 v01.007.000, the optional **`no-rt`** preset is in the
per-game settings. It changes the lighting path; it does not guarantee complete
game compatibility or correct graphics in every scene.

## Compatibility and reporting

Game compatibility and performance vary by title, driver and device. Booting a
game does not establish that it is fully playable. The
[upstream compatibility notes](docs/android/STATUS.md) describe upstream testing,
not a new benchmark of KytyPS5 Plus.

When reporting a problem, include the title ID and game version, device/GPU,
selected driver and presets, and a diagnostic ZIP exported from the app.

The application ID is **`com.kytyps5.plus`**. It installs separately from earlier
`com.kytyps5.android` builds. Subsequent Plus builds use the same application ID.

## Building and contributing

See [the Android build guide](docs/android/CONTRIBUTING.md). The Android workflow
runs unit tests, assembles the release APK and audits its native dependencies.
Changes limited to the Android interface may reuse checksum-verified native
libraries only when native sources and native build commands are unchanged.
Build checks do not replace testing on an Android device.

## Credits

- [erickmendes22 / KytyPS5 Plus](https://github.com/erickmendes22/KytyPS5Plus-Android): this fork and Android interface changes.
- [tarek369 / KytyPS5-Android](https://github.com/tarek369/KytyPS5-Android): base Android port.
- [KytyPS5](https://github.com/KytyPS5/KytyPS5): PS5 emulation core.
- [Kyty](https://github.com/InoriRus/Kyty) by InoriRus: original project.
- [FEX-Emu](https://github.com/FEX-Emu/FEX): FEXCore CPU translation.
- [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html): open-source Adreno Vulkan driver.
- [whitebelyash / AdrenoToolsDrivers](https://github.com/whitebelyash/AdrenoToolsDrivers): bundled Mainline Turnip V32 package.
- [libadrenotools](https://github.com/bylaws/libadrenotools): custom Android GPU driver loading.
- [SDL3](https://github.com/libsdl-org/SDL): windowing, input and audio.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) and all upstream contributors.

No game files, keys or Sony system software are included. This project is not
affiliated with Sony Interactive Entertainment or PlayStation.

## License

[GNU General Public License version 2](LICENSE) (`GPL-2.0-only`). Kyty's original
MIT notice is preserved in [LICENSES/Kyty-MIT.txt](LICENSES/Kyty-MIT.txt).
Third-party components retain their respective licenses.
