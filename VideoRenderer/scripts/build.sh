#!/usr/bin/env bash
#
# VideoRenderer build helper.
#
# Usage:
#   scripts/build.sh             release build (default): configure + build + report
#   scripts/build.sh debug       debug build
#   scripts/build.sh configure   configure only
#   scripts/build.sh build       build only (configures first if needed)
#   scripts/build.sh clean       wipe build/<preset>
#   scripts/build.sh -h|--help
#
# Environment:
#   BUILD_PRESET   release | debug
#   JOBS=N         parallel jobs (default: available logical CPUs)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

if [[ -t 1 ]]; then
    C_CYAN=$'\033[1;36m'; C_GRN=$'\033[1;32m'; C_RED=$'\033[1;31m'; C_YLW=$'\033[1;33m'; C_OFF=$'\033[0m'
else
    C_CYAN=''; C_GRN=''; C_RED=''; C_YLW=''; C_OFF=''
fi
info() { printf '\n%s==>%s %s\n' "$C_CYAN" "$C_OFF" "$*"; }
good() { printf '%sOK:%s %s\n'  "$C_GRN" "$C_OFF" "$*"; }
warn() { printf '%sWARN:%s %s\n' "$C_YLW" "$C_OFF" "$*" >&2; }
die()  { printf '%sERROR:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
VideoRenderer build helper.

Usage:
  scripts/build.sh             configure + build + report (release, default)
  scripts/build.sh debug       configure + build + report (debug)
  scripts/build.sh configure   configure only
  scripts/build.sh build       build only (configures first if needed)
  scripts/build.sh clean       wipe build/<preset>
  scripts/build.sh -h|--help   show this

Environment:
  BUILD_PRESET   release | debug
  JOBS=N         parallel jobs (default: available logical CPUs)
EOF
}

ACTION="all"
POSITIONAL_PRESET=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        configure|build|all|clean) ACTION="$1"; shift ;;
        release|debug) POSITIONAL_PRESET="$1"; shift ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

PRESET="${BUILD_PRESET:-${POSITIONAL_PRESET:-release}}"
case "$PRESET" in
    release|debug) ;;
    *) die "unknown preset: $PRESET (expected release or debug)" ;;
esac
BUILD_DIR="$PROJECT_DIR/build/$PRESET"

command -v cmake >/dev/null || die "cmake not found"
command -v ninja >/dev/null || die "ninja not found"
command -v pkg-config >/dev/null || die "pkg-config not found"

SYSTEM_NAME="$(uname -s)"
case "$SYSTEM_NAME" in
    Darwin)
        xcrun --find clang >/dev/null 2>&1 || die "Xcode CLT not found: xcode-select --install"
        DEFAULT_JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)"
        ;;
    Linux)
        DEFAULT_JOBS="$(nproc 2>/dev/null || echo 8)"
        ;;
    *)
        die "unsupported operating system: $(uname -s)"
        ;;
esac

JOBS="${JOBS:-$DEFAULT_JOBS}"

do_clean() {
    [[ -d "$BUILD_DIR" ]] || { info "nothing to clean ($BUILD_DIR absent)"; return; }
    info "cleaning $BUILD_DIR"; rm -rf "$BUILD_DIR"
}

do_configure() {
    if [[ "$SYSTEM_NAME" == "Darwin" ]]; then
        # The Apple transcoder is linked to the pinned decoder bundle. Linux
        # uses the libmpv backend and must not invoke the macOS-only xcrun flow.
        FFMPEG_ARCH="$(uname -m)"
        FFMPEG_PREFIX="${MIRAGE_FFMPEG_DIR:-$PROJECT_DIR/../Mirage/build/ffmpeg/$FFMPEG_ARCH}"
        "$PROJECT_DIR/../scripts/build_ffmpeg.sh" "$FFMPEG_ARCH"
        [[ -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ]] || die "bundled FFmpeg missing at $FFMPEG_PREFIX (run scripts/build_ffmpeg.sh)"
        export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi
    info "configuring preset: $PRESET"
    cmake -U '*VR_AV*' -U '*VR_SW*' --preset "$PRESET"
}

do_build() {
    do_configure
    info "building preset: $PRESET (jobs=$JOBS)"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
}

report() {
    info "build artifacts:"
    local bin found=0
    for bin in "$BUILD_DIR/Tools/VideoViewer/VideoViewer" \
               "$BUILD_DIR/Tools/VideoWallpaper/VideoWallpaper"; do
        if [[ -x "$bin" ]]; then printf '  %s\n' "$bin"; found=1; fi
    done
    if [[ $found -eq 0 ]]; then
        warn "no VideoViewer/VideoWallpaper binaries found under $BUILD_DIR."
    else
        printf '\nRun e.g.:\n  %s/Tools/VideoViewer/VideoViewer <path/to/video/wallpaper/dir>\n' "$BUILD_DIR"
    fi
}

case "$ACTION" in
    clean)     do_clean ;;
    configure) do_configure ;;
    build)     do_build; report ;;
    all)       do_build; report ;;
esac

good "done."
