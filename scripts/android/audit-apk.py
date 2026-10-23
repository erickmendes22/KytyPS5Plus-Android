#!/usr/bin/env python3
"""Verify the APK ships the Android emulator, its native dependency closure and Turnip.

This audits packaging, not game compatibility or device runtime behavior.
Usage: python3 scripts/android/audit-apk.py path/to/app-release.apk
"""
from __future__ import annotations

import io
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

EXPECTED = {
    "libmain.so",
    "libSDL3.so",
    "libhook_impl.so",
    "libmain_hook.so",
    "libfile_redirect_hook.so",
    "libgsl_alloc_hook.so",
}
ANDROID_SYSTEM = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
    "libOpenSLES.so", "libGLESv1_CM.so", "libGLESv2.so", "libEGL.so",
    "libvulkan.so", "libz.so", "libjnigraphics.so", "libmediandk.so",
    "libaaudio.so",
}
EMBEDDED_FEATURES = {
    "FEXCore guest JIT": b"FEXCore",
    "SPIR-V tools": b"spvtools",
    "FFmpeg codecs": b"avcodec_",
    "ZArchive": b"ZArchiveReader",
    "SDL entrypoint": b"SDL_main",
}

def audit(apk: Path) -> None:
    if not apk.is_file():
        raise RuntimeError(f"APK missing: {apk}")
    with zipfile.ZipFile(apk) as package:
        paths = set(package.namelist())
        libs = {p.rsplit("/", 1)[-1]: p for p in paths if p.startswith("lib/arm64-v8a/") and p.endswith(".so")}
        missing = EXPECTED - set(libs)
        if missing:
            raise RuntimeError(f"Missing emulator libraries: {sorted(missing)}")
        unexpected_abis = sorted(p for p in paths if p.startswith("lib/") and not p.startswith("lib/arm64-v8a/"))
        if unexpected_abis:
            raise RuntimeError(f"Unexpected ABI files: {unexpected_abis}")
        print(f"APK: {apk} ({apk.stat().st_size:,} bytes)")
        with tempfile.TemporaryDirectory() as tmp:
            for name, path in sorted(libs.items()):
                binary = package.read(path)
                output = Path(tmp) / name
                output.write_bytes(binary)
                result = subprocess.run(["readelf", "-d", str(output)], capture_output=True, text=True, check=True)
                deps = re.findall(r"Shared library: \[([^]]+)\]", result.stdout)
                unresolved = sorted(set(deps) - set(libs) - ANDROID_SYSTEM)
                print(f"  {name}: {len(binary):,} bytes, NEEDED={deps}")
                if unresolved:
                    raise RuntimeError(f"{name} has unpackaged non-system libraries: {unresolved}")
            core = package.read(libs["libmain.so"])
            for feature, marker in EMBEDDED_FEATURES.items():
                if marker not in core:
                    raise RuntimeError(f"Native core may be incomplete: missing {feature} marker")
                print(f"  Embedded: {feature}")
        driver_paths = [p for p in paths if p.startswith("assets/gpu_drivers/") and p.lower().endswith(".zip")]
        if not driver_paths:
            raise RuntimeError("Bundled GPU driver missing (Turnip zip not packaged)")
        for path in sorted(driver_paths):
            with zipfile.ZipFile(io.BytesIO(package.read(path))) as driver:
                bad = driver.testzip()
                if bad is not None:
                    raise RuntimeError(f"Corrupt driver package: {path}: {bad}")
                if not any(e.endswith(".so") for e in driver.namelist()):
                    raise RuntimeError(f"No Vulkan shared library in {path}")
                print(f"  GPU driver: {path}: {driver.namelist()}")
        if "classes.dex" not in paths:
            raise RuntimeError("Android launcher classes.dex missing")
        print("PASS: native libraries, dependency closure, embedded components and GPU driver")
        print("NOTE: PS5 game detection/boot still requires an on-device test")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: audit-apk.py <apk>")
    try:
        audit(Path(sys.argv[1]))
    except (OSError, zipfile.BadZipFile, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
