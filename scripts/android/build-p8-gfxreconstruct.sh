#!/usr/bin/env bash
set -euo pipefail
ndk="$1"
out="$2"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
pin=4401228c791b11fb7ea15b93795cbd1e938307c0
git clone --depth 1 --branch vulkan-sdk-1.4.363.0 --recurse-submodules --shallow-submodules https://github.com/LunarG/gfxreconstruct.git "$work/src"
test "$(git -C "$work/src" rev-parse HEAD)" = "$pin"
# Android capture properties ordinarily require adb shell. Permit the app
# itself to configure only its capture layer, without privileged properties.
python3 - "$work/src/framework/util/platform.h" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
s=p.read_text()
needle="    const prop_info* pi = __system_property_find(name);"
assert s.count(needle)==1
s=s.replace(needle,'''    if (std::string(name).find("debug.gfxrecon.") == 0) {
        const char* app_value = ::getenv(name);
        if (app_value != nullptr) return std::string(app_value);
    }
'''+needle)
p.write_text(s)
PY
cmake -S "$work/src/android/layer" -B "$work/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DGFXRECON_ENABLE_OPENXR=OFF
cmake --build "$work/build" --target VkLayer_gfxreconstruct --parallel 2
mkdir -p "$out"
lib="$(find "$work/build" -name libVkLayer_gfxreconstruct.so -type f -print -quit)"
test -n "$lib"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded -o "$out/libVkLayer_gfxreconstruct.so" "$lib"
sha256sum "$out/libVkLayer_gfxreconstruct.so"
