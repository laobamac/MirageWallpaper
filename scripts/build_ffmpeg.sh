#!/bin/bash
set -euo pipefail
COPYRIGHT="Copyright © 2026 王孝慈. All rights reserved."

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${1:-$(uname -m)}"
VERSION="8.1.2"
SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
OUTPUT="${MIRAGE_FFMPEG_DIR:-$ROOT/Mirage/build/ffmpeg/$ARCH}"
WORK="$ROOT/Mirage/build/ffmpeg/work-$ARCH"
ARCHIVE="$ROOT/Mirage/build/ffmpeg/ffmpeg-$VERSION.tar.xz"
STAMP="$OUTPUT/.mirage-ffmpeg"
SCRIPT_HASH="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
CLANG="$(xcrun --find clang)"
CC="$CLANG -isysroot $SDK"

case "$ARCH" in
    arm64|x86_64) ;;
    *) echo "[ffmpeg] unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

[ "$ARCH" = "$(uname -m)" ] || { echo "[ffmpeg] build on a native $ARCH runner" >&2; exit 1; }
DAV1D_PREFIX="$(brew --prefix dav1d)"
BUILD_ID="$VERSION $SCRIPT_HASH $ARCH $MACOSX_DEPLOYMENT_TARGET $(xcrun --sdk macosx --show-sdk-version) $($CLANG --version | head -1) $(PKG_CONFIG_PATH="$DAV1D_PREFIX/lib/pkgconfig" pkg-config --modversion dav1d) $OUTPUT"
case "$OUTPUT" in
    "$ROOT"/Mirage/build/ffmpeg/"$ARCH") ;;
    *) echo "[ffmpeg] output must be $ROOT/Mirage/build/ffmpeg/$ARCH" >&2; exit 1 ;;
esac
[ ! -L "$OUTPUT" ] && [ ! -L "$WORK" ] || { echo "[ffmpeg] symlink build directories are not supported" >&2; exit 1; }
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$BUILD_ID" ] && [ -f "$OUTPUT/lib/pkgconfig/libavcodec.pc" ]; then
    echo "[ffmpeg] up to date: $OUTPUT"
    exit 0
fi

command -v pkg-config >/dev/null || { echo "[ffmpeg] pkg-config not found. brew install pkg-config" >&2; exit 1; }
command -v make >/dev/null || { echo "[ffmpeg] make not found. xcode-select --install" >&2; exit 1; }
if [ "$ARCH" = "x86_64" ]; then
    command -v nasm >/dev/null || { echo "[ffmpeg] nasm not found. brew install nasm" >&2; exit 1; }
fi
DAV1D_PREFIX="$(brew --prefix dav1d 2>/dev/null || true)"
[ -f "$DAV1D_PREFIX/lib/pkgconfig/dav1d.pc" ] || { echo "[ffmpeg] dav1d not found. brew install dav1d" >&2; exit 1; }
export PKG_CONFIG_PATH="$DAV1D_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

mkdir -p "$(dirname "$ARCHIVE")"
if [ ! -f "$ARCHIVE" ] || [ "$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)" != "$SHA256" ]; then
    echo "[ffmpeg] downloading ffmpeg-$VERSION"
    curl -fsSL -o "$ARCHIVE.tmp" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    mv -f "$ARCHIVE.tmp" "$ARCHIVE"
fi
[ "$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)" = "$SHA256" ] || { echo "[ffmpeg] checksum mismatch: $ARCHIVE" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xJf "$ARCHIVE" -C "$WORK"
SOURCE="$WORK/ffmpeg-$VERSION"

HWACCELS="h264_videotoolbox,hevc_videotoolbox,vp9_videotoolbox,av1_videotoolbox,mpeg2_videotoolbox,mpeg4_videotoolbox,prores_videotoolbox"

CONFIGURE_ARGS=(
    --prefix="$OUTPUT"
    --arch="$ARCH"
    --cc="$CC"
    --host-cc="$CC"
    --sysroot="$SDK"
    --extra-cflags="-isysroot $SDK -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -arch $ARCH"
    --extra-ldflags="-isysroot $SDK -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -arch $ARCH"
    --enable-shared
    --disable-static
    --disable-programs
    --disable-doc
    --disable-debug
    --disable-avdevice
    --disable-avfilter
    --disable-network
    --disable-autodetect
    --disable-everything
    --disable-encoders
    --disable-muxers
    --disable-devices
    --disable-filters
    --disable-securetransport
    --disable-audiotoolbox
    --disable-coreimage
    --disable-metal
    --disable-appkit
    --disable-avfoundation
    --disable-sdl2
    --disable-xlib
    --disable-libxcb
    --disable-bzlib
    --disable-lzma
    --disable-iconv
    --enable-zlib
    --enable-videotoolbox
    --enable-libdav1d
    --enable-demuxers
    --enable-decoders
    --enable-parsers
    --enable-hwaccel="$HWACCELS"
    --enable-bsfs
    --enable-protocol=file
)
if [ "$ARCH" = "x86_64" ]; then
    CONFIGURE_ARGS+=(--x86asmexe="$(command -v nasm)")
fi

echo "[ffmpeg] configuring ffmpeg-$VERSION for $ARCH"
(cd "$SOURCE" && ./configure "${CONFIGURE_ARGS[@]}" > "$WORK/configure.log" 2>&1) || {
    tail -40 "$WORK/configure.log" >&2
    [ -f "$SOURCE/ffbuild/config.log" ] && tail -40 "$SOURCE/ffbuild/config.log" >&2
    exit 1
}
echo "[ffmpeg] building ($JOBS jobs)"
(cd "$SOURCE" && make -j"$JOBS" > "$WORK/build.log" 2>&1) || { tail -60 "$WORK/build.log" >&2; exit 1; }
(cd "$SOURCE" && make install DESTDIR="$WORK/stage" > "$WORK/install.log" 2>&1) || { tail -40 "$WORK/install.log" >&2; exit 1; }

STAGED="$WORK/stage$OUTPUT"
for library in avcodec avformat avutil swscale swresample; do
    test -f "$STAGED/lib/lib$library.dylib"
    lipo "$STAGED/lib/lib$library.dylib" -verify_arch "$ARCH"
done

rm -rf "$OUTPUT"
mv "$STAGED" "$OUTPUT"
mkdir -p "$OUTPUT/Licenses"
cp -f "$SOURCE/COPYING.LGPLv2.1" "$OUTPUT/Licenses/FFmpeg-LGPL-2.1.txt"
cp -f "$SOURCE/LICENSE.md" "$OUTPUT/Licenses/FFmpeg-LICENSE.md"
{
    echo "FFmpeg $VERSION"
    echo "source: https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    echo "sha256: $SHA256"
    echo "configure:"
    printf '  %s\n' "${CONFIGURE_ARGS[@]}"
} > "$OUTPUT/Licenses/FFmpeg-BUILD.txt"
rm -rf "$OUTPUT/share"
find "$OUTPUT/lib" -type f -name '*.dylib' -exec strip -x {} +
rm -rf "$WORK"
echo "$BUILD_ID" > "$STAMP"
echo "[ffmpeg] installed: $OUTPUT"
du -sh "$OUTPUT/lib"
