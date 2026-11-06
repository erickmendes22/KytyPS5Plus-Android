# Incremental Android native builds

Based on the TLOU workflow pattern in erickmendes22/tlou-remastered-android, branch tlou-ui-approval-log32-20261105, .github/workflows/tlou-native-apk.yml: preserve dependency/FEX builds and a separate incremental compiler cache, and reuse an audited payload for interface-only changes.

Kyty now restores the FEX source, compiled libraries and ExternalProject stamps under an NDK/ABI/configuration/patch key. Compiler launchers are explicitly passed into the separate FEX CMake project; previously ENABLE_CCACHE=FALSE prevented FEX's own cache setup and no launcher was passed. The main incremental ccache remains enabled. Pinned dependency source downloads and Gradle dependencies/scripts are cached separately.

A complete ARM64 native payload is also cached after the APK dependency and ABI audits pass. The key covers tracked native sources, submodule revisions, native Android sources, NDK version, native build command blocks and runner image. UI, documentation and tests do not change a native payload key. Compatibility and per-library SHA-256/ARM64 ELF checks are required before reusing it. The generated native-core.json records the original core source commit, so an interface build does not claim to contain newly compiled native code.

The old fixed-APK pin in reuse_native.py is no longer the workflow's reuse mechanism. A cache miss runs the native build with component/object reuse; a changed native input cannot select an older compatible-key payload. Native regressions and final Android unit/package audits remain required. The first run populates the new FEX and native-payload caches; subsequent matching builds reuse them.

Unit coverage checks UI-only compatibility, native/source/submodule/NDK/build-command invalidation, payload integrity and architecture rejection. No build-time savings are claimed before corresponding cache hits are observed in CI.
