#!/usr/bin/env python3
"""Run the actual Android file-closure selector without an Android device.

Needs a JDK and either org.json:json:20180813.jar or its checked-out Java sources.
No downloads or APK builds occur. Optional --capture directories run assertions
against the extracted #741/#744 historical evidence and print the transport
inventory, including exact missing files. This test is not a rendering oracle.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    dependency = parser.add_mutually_exclusive_group(required=True)
    dependency.add_argument("--json-jar", type=Path)
    dependency.add_argument("--json-sources", type=Path,
                            help="JSON-java checkout containing JSONObject.java")
    parser.add_argument("--capture", type=Path, action="append", default=[],
                        help="extracted ds-lighting-capture directory (#741/#744)")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    source = repo / "android/app/src/main/java/com/kytyps5/android/DsLightingCaptureExport.java"
    fixtures = repo / "android/app/src/test/java/com/kytyps5/android/DsLightingCaptureExportFixtures.java"
    java = shutil.which("java")
    if java is None:
        parser.error("a JDK with java is required")
    javac = shutil.which("javac")
    compiler = [javac] if javac else [java, "com.sun.tools.javac.Main"]
    source_files = [str(source), str(fixtures)]
    if args.json_sources:
        dependency_sources = sorted(args.json_sources.glob("*.java"))
        if not (args.json_sources / "JSONObject.java").is_file():
            parser.error("--json-sources must contain JSONObject.java")
        source_files.extend(str(path) for path in dependency_sources)
    if args.json_jar and not args.json_jar.is_file():
        parser.error("--json-jar does not exist")
    for capture in args.capture:
        if not (capture / "queue-state").is_dir():
            parser.error(f"not an extracted capture root: {capture}")
    with tempfile.TemporaryDirectory(prefix="ds-export-classes-") as temporary:
        classpath = temporary
        if args.json_jar:
            classpath += os.pathsep + str(args.json_jar.resolve())
        # -source/-target also work on JDK distributions without ct.sym.
        subprocess.run(compiler + ["-source", "17", "-target", "17", "-cp", classpath,
                                   "-d", temporary] + source_files, check=True)
        subprocess.run([java, "-cp", classpath,
                        "com.kytyps5.android.DsLightingCaptureExportFixtures"] +
                       [str(path.resolve()) for path in args.capture], check=True)


if __name__ == "__main__":
    main()
