#!/usr/bin/env bash
#
# WebRenderer build helper. macOS uses Apple frameworks; Linux uses QtWebEngine
# and the repository-maintained MirageLinuxDisplay Vulkan producer.
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
#   JOBS=N         parallel jobs (default: hw.logicalcpu)

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
WebRenderer macOS build helper.

Usage:
  scripts/build.sh             configure + build + report (release, default)
  scripts/build.sh debug       configure + build + report (debug)
  scripts/build.sh configure   configure only
  scripts/build.sh build       build only (configures first if needed)
  scripts/build.sh clean       wipe build/<preset>
  scripts/build.sh -h|--help   show this

Environment:
  BUILD_PRESET   release | debug
  JOBS=N         parallel jobs (default: hw.logicalcpu)
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
command -v cmake >/dev/null || die "cmake not found. brew install cmake"
command -v ninja >/dev/null || die "ninja not found. brew install ninja"
OS_NAME="$(uname -s)"
if [[ "$OS_NAME" == "Darwin" ]]; then
    xcrun --find clang >/dev/null 2>&1 || die "Xcode CLT not found: xcode-select --install"
    PRESET_NAME="$PRESET"
    JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)}"
elif [[ "$OS_NAME" == "Linux" ]]; then
    PRESET_NAME="linux-$PRESET"
    JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"
else
    die "unsupported host platform: $OS_NAME"
fi
BUILD_DIR="$PROJECT_DIR/build/$PRESET_NAME"

do_clean() {
    [[ -d "$BUILD_DIR" ]] || { info "nothing to clean ($BUILD_DIR absent)"; return; }
    info "cleaning $BUILD_DIR"; rm -rf "$BUILD_DIR"
}

do_configure() {
    info "configuring preset: $PRESET_NAME"
    cmake --preset "$PRESET_NAME"
}

do_build() {
    [[ -f "$BUILD_DIR/CMakeCache.txt" ]] || do_configure
    info "building preset: $PRESET_NAME (jobs=$JOBS)"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
}

report() {
    info "build artifacts:"
    local bin found=0
    for bin in "$BUILD_DIR/Tools/WebViewer/WebViewer" \
               "$BUILD_DIR/Tools/WebWallpaper/WebWallpaper"; do
        if [[ -x "$bin" ]]; then printf '  %s\n' "$bin"; found=1; fi
    done
    if [[ $found -eq 0 ]]; then
        warn "no WebViewer/WebWallpaper binaries found under $BUILD_DIR."
    else
        printf '\nRun e.g.:\n  %s/Tools/WebViewer/WebViewer <path/to/web/wallpaper/dir>\n' "$BUILD_DIR"
    fi
}

case "$ACTION" in
    clean)     do_clean ;;
    configure) do_configure ;;
    build)     do_build; report ;;
    all)       do_configure; do_build; report ;;
esac

good "done."
