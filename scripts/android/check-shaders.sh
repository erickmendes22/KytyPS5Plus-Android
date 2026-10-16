#!/usr/bin/env bash
# Compiles every pipeline a game produced on the device's Vulkan driver, without playing it.
#
# 1. Dump while playing (opt-in):  GAME_ARGS '--game,<path>,--dump-shaders,_ShaderDump_<name>'
#    The app writes files/_ShaderDump_<name>/{device.json,spv/,pipelines/,pipelines.log}.
# 2. Check:  check-shaders.sh [options] <name|local-dump-dir> [-- shader_checker args]
#
# A <name> is pulled from the app (debug build, run-as) into <build-dir>/shader-dumps/<name>
# first; a directory is used as is. The dump and a stripped shader_checker are pushed to
# $DEVICE_DIR, checked there, and removed again. The output lists each pipeline the driver
# rejected (FAIL), crashed on (CRASH) or hung on (TIMEOUT) with the guest shader hashes, plus
# the driver's own log lines (Adreno: "Failed to link shaders", ...).
#
# Options:
#   --build <dir>    Android build dir containing shader_checker (default: _Build/android)
#   --no-pull        <name> must already be in <build-dir>/shader-dumps
#   --keep           leave the pushed files on the device
# Environment:
#   ANDROID_SERIAL   device to use
#   DEVICE_DIR       scratch dir on the device (default /data/local/tmp/kyty-shadercheck)
#   DEVICE_LOCK      optional lock command serializing device use, called as
#                    "$DEVICE_LOCK acquire|release $DEVICE_LOCK_NAME"
#   APP_ID           application id (default com.kytyps5.android)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/_Build/android"
PULL=1
KEEP=0
APP_ID="${APP_ID:-com.kytyps5.android}"
DEVICE_DIR="${DEVICE_DIR:-/data/local/tmp/kyty-shadercheck}"
NDK="${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/28.2.13676358}"

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

TARGET=""
while [ $# -gt 0 ]; do
	case "$1" in
		--build) BUILD="$2"; shift 2 ;;
		--no-pull) PULL=0; shift ;;
		--keep) KEEP=1; shift ;;
		-h|--help) usage ;;
		--) shift; break ;;
		*) [ -z "$TARGET" ] || usage; TARGET="$1"; shift ;;
	esac
done
[ -n "$TARGET" ] || usage

CHECKER="$BUILD/shader_checker"
if [ ! -x "$CHECKER" ]; then
	echo "building shader_checker in $BUILD" >&2
	ninja -C "$BUILD" shader_checker >&2
fi
case "$(uname -s)" in
	Darwin) STRIP="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" ;;
	*) STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" ;;
esac
STRIPPED="$BUILD/shader_checker.stripped"
if [ ! -f "$STRIPPED" ] || [ "$CHECKER" -nt "$STRIPPED" ]; then
	"$STRIP" -o "$STRIPPED" "$CHECKER"
fi

lock()   { [ -z "${DEVICE_LOCK:-}" ] || $DEVICE_LOCK acquire ${DEVICE_LOCK_NAME:-} >&2; }
unlock() { [ -z "${DEVICE_LOCK:-}" ] || $DEVICE_LOCK release ${DEVICE_LOCK_NAME:-} >&2; }
cleanup() {
	if [ "$KEEP" = 0 ]; then adb shell rm -rf "$DEVICE_DIR" >/dev/null 2>&1 || true; fi
	unlock
}

if [ -d "$TARGET" ]; then
	DUMP="$(cd "$TARGET" && pwd)"
else
	DUMP="$BUILD/shader-dumps/$TARGET"
	if [ "$PULL" = 1 ]; then
		lock
		trap unlock EXIT
		rm -rf "$DUMP.tmp" && mkdir -p "$DUMP.tmp"
		adb exec-out run-as "$APP_ID" tar cf - -C files "_ShaderDump_$TARGET" | tar xf - -C "$DUMP.tmp"
		rm -rf "$DUMP" && mv "$DUMP.tmp/_ShaderDump_$TARGET" "$DUMP" && rm -rf "$DUMP.tmp"
		unlock
		trap - EXIT
	fi
fi
[ -d "$DUMP/pipelines" ] || { echo "no pipelines/ in $DUMP" >&2; exit 2; }
echo "dump: $DUMP ($(ls "$DUMP/pipelines" | wc -l | tr -d ' ') pipelines, $(ls "$DUMP/spv" | wc -l | tr -d ' ') modules)"

lock
trap cleanup EXIT
adb shell "rm -rf $DEVICE_DIR && mkdir -p $DEVICE_DIR/dump" >/dev/null
adb push "$STRIPPED" "$DEVICE_DIR/shader_checker" >/dev/null
adb push "$DUMP/spv" "$DUMP/pipelines" "$DEVICE_DIR/dump/" >/dev/null
adb shell chmod 755 "$DEVICE_DIR/shader_checker"
adb logcat -c || true
set +e
adb shell "cd $DEVICE_DIR && ./shader_checker $* dump"
STATUS=$?
set -e
# Driver diagnostics only (Adreno logs "Failed to link shaders." / "Pipeline create failed").
DRIVER_LOG="$(adb logcat -d 2>/dev/null |
	grep -E "(Adreno|AdrenoVK|mesa|turnip|vulkan)[^:]*: .*([Ff]ail|[Ee]rror|[Cc]rash|abort)" |
	grep -v "QSPM-HAL" || true)"
if [ -n "$DRIVER_LOG" ]; then
	echo
	echo "driver log:"
	echo "$DRIVER_LOG" | tail -40
fi
exit "$STATUS"
