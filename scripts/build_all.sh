#!/usr/bin/env bash
#
# MirageWallpaper — one-shot build script for macOS.
#
# Builds all components in dependency order:
#   1. SceneRenderer  → SceneWallpaper   (Homebrew Clang + MoltenVK)
#   2. WebRenderer    → WebWallpaper     (system frameworks)
#   3. VideoRenderer  → VideoWallpaper   (system frameworks)
#   4. RmskinRenderer → RmskinWallpaper  (system frameworks)
#   5. Mirage         → Mirage.app       (xcodebuild), embedding the four renderers
#
# Each sub-project already has its own scripts/build.sh; this script
# orchestrates them, passing through arguments and environment variables
# where the sub-scripts expect them.
#
# Usage:
#   scripts/build_all.sh                 full build (release, default): renderers + App
#   scripts/build_all.sh debug           debug build
#   scripts/build_all.sh renderers       build only the four renderers (skip App)
#   scripts/build_all.sh app             build only Mirage App (assumes renderers are ready)
#   scripts/build_all.sh scene|web|video|rmskin build a single named renderer
#   scripts/build_all.sh clean           remove all sub-project build directories
#   scripts/build_all.sh -h|--help
#
# Environment variables:
#   JOBS=N                      parallel build jobs (default: hw.logicalcpu), passed to renderers
#   MIRAGE_ARCH=arm64|x86_64    Mirage App target architecture (default: current host arch)
#   MIRAGE_STEAM_WEB_API_KEY    optional built-in Steam Web API Key (32 hex chars)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- terminal colors (disabled when not a TTY) ---
if [[ -t 1 ]]; then
    C_CYAN=$'\033[1;36m'; C_GRN=$'\033[1;32m'; C_RED=$'\033[1;31m'; C_YLW=$'\033[1;33m'; C_MAG=$'\033[1;35m'; C_OFF=$'\033[0m'
else
    C_CYAN=''; C_GRN=''; C_RED=''; C_YLW=''; C_MAG=''; C_OFF=''
fi
step() { printf '\n%s========== %s ==========%s\n' "$C_MAG" "$*" "$C_OFF"; }
info() { printf '%s==>%s %s\n' "$C_CYAN" "$C_OFF" "$*"; }
good() { printf '%sOK:%s %s\n'  "$C_GRN" "$C_OFF" "$*"; }
warn() { printf '%sWARN:%s %s\n' "$C_YLW" "$C_OFF" "$*" >&2; }
die()  { printf '%sERROR:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
MirageWallpaper one-shot build script for macOS.

Usage:
  scripts/build_all.sh                 full build (release, default): renderers + App
  scripts/build_all.sh debug           debug build
  scripts/build_all.sh renderers       build only the four renderers (skip App)
  scripts/build_all.sh app             build only Mirage App (assumes renderers are ready)
  scripts/build_all.sh scene           build SceneRenderer only
  scripts/build_all.sh web             build WebRenderer only
  scripts/build_all.sh video           build VideoRenderer only
  scripts/build_all.sh rmskin          build RmskinRenderer only
  scripts/build_all.sh clean           remove all sub-project build directories
  scripts/build_all.sh -h|--help       show this help

Environment variables:
  JOBS=N                      parallel build jobs (default: hw.logicalcpu)
  MIRAGE_ARCH=arm64|x86_64    Mirage App target architecture (default: current host arch)
  MIRAGE_STEAM_WEB_API_KEY    optional built-in Steam Web API Key (32 hex chars)
EOF
}

# --- argument parsing ---
TARGET="all"
CONFIG="release"      # lowercase preset name passed to renderers (release|debug)
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        all|renderers|app|scene|web|video|rmskin|clean) TARGET="$1"; shift ;;
        release|debug) CONFIG="$1"; shift ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

# xcodebuild expects a capitalised configuration name.
if [[ "$CONFIG" == "debug" ]]; then
    XCODE_CONFIG="Debug"
else
    XCODE_CONFIG="Release"
fi

# --- platform check ---
[[ "$(uname -s)" == "Darwin" ]] || die "This script only supports macOS."

SCENE_SH="$ROOT_DIR/SceneRenderer/scripts/build.sh"
WEB_SH="$ROOT_DIR/WebRenderer/scripts/build.sh"
VIDEO_SH="$ROOT_DIR/VideoRenderer/scripts/build.sh"
RMSKIN_SH="$ROOT_DIR/RmskinRenderer/scripts/build.sh"
MIRAGE_SH="$ROOT_DIR/Mirage/scripts/build.sh"

for s in "$SCENE_SH" "$WEB_SH" "$VIDEO_SH" "$RMSKIN_SH" "$MIRAGE_SH"; do
    [[ -f "$s" ]] || die "missing sub-script: $s"
done

# --- per-component build helpers ---
build_scene() {
    step "Building SceneRenderer ($CONFIG)"
    bash "$SCENE_SH" "$CONFIG"
}
build_web() {
    step "Building WebRenderer ($CONFIG)"
    bash "$WEB_SH" "$CONFIG"
}
build_video() {
    step "Building VideoRenderer ($CONFIG)"
    bash "$VIDEO_SH" "$CONFIG"
}
build_rmskin() {
    step "Building RmskinRenderer ($CONFIG)"
    bash "$RMSKIN_SH" "$CONFIG"
}
build_renderers() {
    build_scene
    build_web
    build_video
    build_rmskin
}
build_app() {
    step "Building Mirage App ($XCODE_CONFIG)"
    bash "$MIRAGE_SH" "$XCODE_CONFIG"
}

# --- clean: remove all sub-project build dirs ---
clean_all() {
    step "Cleaning all build directories"
    bash "$SCENE_SH" clean "$CONFIG" || true
    bash "$WEB_SH"   clean "$CONFIG" || true
    bash "$VIDEO_SH" clean "$CONFIG" || true
    bash "$RMSKIN_SH" clean "$CONFIG" || true
    if [[ -d "$ROOT_DIR/Mirage/build" ]]; then
        info "Removing Mirage/build"
        rm -rf "$ROOT_DIR/Mirage/build"
    fi
    if [[ -d "$ROOT_DIR/Mirage/dist" ]]; then
        info "Removing Mirage/dist"
        rm -rf "$ROOT_DIR/Mirage/dist"
    fi
}

# --- dispatch ---
case "$TARGET" in
    scene)     build_scene ;;
    web)       build_web ;;
    video)     build_video ;;
    rmskin)    build_rmskin ;;
    renderers) build_renderers ;;
    app)       build_app ;;
    clean)     clean_all ;;
    all)       build_renderers; build_app ;;
esac

if [[ "$TARGET" == "all" || "$TARGET" == "app" ]]; then
    APP="$ROOT_DIR/Mirage/dist/Mirage.app"
    step "Build complete"
    if [[ -d "$APP" ]]; then
        good "artifact: $APP"
    else
        warn "App artifact not found: $APP"
    fi
else
    good "done."
fi
