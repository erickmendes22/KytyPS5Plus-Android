#!/usr/bin/env bash
# Builds the KytyPS5 FFmpeg subset (same component list as the desktop
# ext-ffmpeg-core recipe) as static libraries for Android arm64-v8a.
#
# Output: <build-dir>/install/{include,lib} which can be passed to CMake as
#   -DFFMPEG_PREBUILT_DIR=<build-dir>/install
#
# Usage: build-ffmpeg-android.sh <build-dir> [android-api]
set -euo pipefail

BUILD_DIR="${1:?usage: $0 <build-dir> [android-api]}"
API="${2:-31}"
FFMPEG_RELEASE="9ac4cfd195f1"
SOURCE_NAME="ffmpeg-source-linux-arm64.tar.gz"
SOURCE_SHA256="f335006f66e9e10421aca9ac3ad8a7d3c5ec3730f8aa11bd2ec1d1139ddedaa8"
NDK="${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/28.2.13676358}"

case "$(uname -s)-$(uname -m)" in
	Darwin-*) HOST_TAG="darwin-x86_64" ;;
	Linux-*) HOST_TAG="linux-x86_64" ;;
	*) echo "unsupported host" >&2; exit 1 ;;
esac

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CC="$TOOLCHAIN/aarch64-linux-android${API}-clang"
CXX="$TOOLCHAIN/aarch64-linux-android${API}-clang++"
[ -x "$CC" ] || { echo "NDK clang not found: $CC" >&2; exit 1; }

mkdir -p "$BUILD_DIR/download" "$BUILD_DIR/src" "$BUILD_DIR/install"
TARBALL="$BUILD_DIR/download/$SOURCE_NAME"
if [ ! -f "$TARBALL" ]; then
	curl -fL -o "$TARBALL" \
		"https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/${FFMPEG_RELEASE}/${SOURCE_NAME}"
fi
echo "$SOURCE_SHA256  $TARBALL" | shasum -a 256 -c -

SRC_DIR="$BUILD_DIR/src/ffmpeg"
if [ ! -f "$SRC_DIR/configure" ]; then
	rm -rf "$BUILD_DIR/src"
	mkdir -p "$BUILD_DIR/src"
	tar -xzf "$TARBALL" -C "$BUILD_DIR/src"
	# The release tarball keeps the patched FFmpeg tree under ffmpeg/.
	[ -f "$SRC_DIR/configure" ] || { echo "unexpected tarball layout" >&2; exit 1; }
fi

# Component list mirrors 3rdparty/ffmpeg-core vcpkg port (portfile.cmake).
COMPONENTS=(
	--enable-decoder=aac --enable-decoder=aac_latm --enable-decoder=atrac3
	--enable-decoder=atrac3p --enable-decoder=atrac9 --enable-decoder=mp3
	--enable-decoder=pcm_s16le --enable-decoder=pcm_s8 --enable-decoder=mov
	--enable-decoder=h264 --enable-decoder=mpeg4 --enable-decoder=mpeg2video
	--enable-decoder=mjpeg --enable-decoder=mjpegb --enable-decoder=hevc
	--enable-decoder=vp8 --enable-decoder=vp9 --enable-decoder=opus
	--enable-decoder=vorbis
	--enable-encoder=pcm_s16le --enable-encoder=ffv1 --enable-encoder=mpeg4
	--enable-encoder=ljpeg --enable-encoder=mjpeg
	--enable-muxer=avi
	--enable-demuxer=h265 --enable-demuxer=h264 --enable-demuxer=m4v
	--enable-demuxer=mp3 --enable-demuxer=mpegvideo --enable-demuxer=mpegps
	--enable-demuxer=mjpeg --enable-demuxer=mov --enable-demuxer=avi
	--enable-demuxer=aac --enable-demuxer=pmp --enable-demuxer=oma
	--enable-demuxer=pcm_s16le --enable-demuxer=pcm_s8 --enable-demuxer=wav
	--enable-demuxer=matroska
	--enable-parser=h264 --enable-parser=mpeg4video --enable-parser=mpegaudio
	--enable-parser=mpegvideo --enable-parser=mjpeg --enable-parser=aac
	--enable-parser=aac_latm --enable-parser=vp8 --enable-parser=vp9
	--enable-parser=opus --enable-parser=vorbis
	--enable-protocol=file
	--enable-bsf=mjpeg2jpeg
)

cd "$BUILD_DIR/src/ffmpeg"
# Same as the port: asm is disabled for Android arm64 (non-PIC relocation issues).
# TODO(perf): re-enable NEON asm once the Android build is validated on device.
./configure \
	--prefix="$BUILD_DIR/install" \
	--enable-cross-compile \
	--target-os=android \
	--arch=aarch64 \
	--cc="$CC" \
	--cxx="$CXX" \
	--ar="$TOOLCHAIN/llvm-ar" \
	--nm="$TOOLCHAIN/llvm-nm" \
	--ranlib="$TOOLCHAIN/llvm-ranlib" \
	--strip="$TOOLCHAIN/llvm-strip" \
	--enable-pic \
	--enable-pthreads \
	--enable-optimizations \
	--enable-runtime-cpudetect \
	--disable-autodetect \
	--disable-everything \
	--disable-doc \
	--disable-programs \
	--disable-avdevice \
	--disable-postproc \
	--disable-asm \
	--disable-x86asm \
	--disable-shared \
	--enable-static \
	"${COMPONENTS[@]}"

make -j"$(getconf _NPROCESSORS_ONLN)"
make install
echo "FFmpeg installed to $BUILD_DIR/install"
