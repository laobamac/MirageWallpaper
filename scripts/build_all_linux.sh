#!/usr/bin/env bash
#
# MirageWallpaper — one-shot build + packaging script for Linux.
#
# Builds all Linux components in dependency order:
#   1. SceneRenderer → SceneWallpaper   (linux-clang-{config} CMake preset)
#   2. VideoRenderer → VideoWallpaper   (release|debug CMake preset)
#   3. MirageQt      → MirageQt app     (CMake, Qt 6.8+)
#
# WebRenderer is macOS-only and the Linux web renderer is not implemented yet,
# so it is skipped on Linux.
#
# Finally everything is staged under dist/ and packed into a self-contained
# tar.gz: the MirageQt binary, the SceneWallpaper / VideoWallpaper
# renderer binaries, the mirage-display shared library (with the renderer
# RUNPATH rewritten to $ORIGIN), the shared assets directory, and a desktop
# entry — a complete distribution you can extract and run directly.
#
# Usage:
#   scripts/build_all_linux.sh                 full build (release, default) + tar.gz
#   scripts/build_all_linux.sh debug           debug build
#   scripts/build_all_linux.sh renderers       build only the renderers (skip app)
#   scripts/build_all_linux.sh app             build only MirageQt (assumes renderers are ready)
#   scripts/build_all_linux.sh scene|video     build a single named renderer
#   scripts/build_all_linux.sh package         stage + pack existing artifacts into dist/*.tar.gz
#   scripts/build_all_linux.sh clean           remove all sub-project build dirs and dist
#   scripts/build_all_linux.sh -h|--help
#
# Environment variables:
#   JOBS=N                       parallel build jobs (default: nproc)
#   MIRAGE_STEAM_WEB_API_KEY     optional built-in Steam Web API Key (32 hex chars)
#   MIRAGEQT_BUILD_DIR=PATH      MirageQt build dir override (default: MirageQt/build/{release,debug})
#   PACKAGE_DIR=PATH             staging root for the tar.gz (default: $ROOT_DIR/dist)
#   VERSION=STRING               package version override (default: git describe)
#   NO_PACKAGE=1                 build without creating the tar.gz

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
MirageWallpaper one-shot build + packaging script for Linux.

Usage:
  scripts/build_all_linux.sh                 full build (release, default) + tar.gz
  scripts/build_all_linux.sh debug           debug build
  scripts/build_all_linux.sh renderers       build only the renderers (skip app)
  scripts/build_all_linux.sh app             build only MirageQt (assumes renderers are ready)
  scripts/build_all_linux.sh scene           build SceneRenderer only
  scripts/build_all_linux.sh video           build VideoRenderer only
  scripts/build_all_linux.sh package         stage + pack existing artifacts into dist/*.tar.gz
  scripts/build_all_linux.sh clean           remove all sub-project build dirs and dist
  scripts/build_all_linux.sh -h|--help       show this help

Environment variables:
  JOBS=N                      parallel build jobs (default: nproc)
  MIRAGE_STEAM_WEB_API_KEY    optional built-in Steam Web API Key (32 hex chars)
  MIRAGEQT_BUILD_DIR=PATH     MirageQt build dir override (default: MirageQt/build/{release,debug})
  PACKAGE_DIR=PATH            staging root for the tar.gz (default: <repo>/dist)
  VERSION=STRING              package version override (default: git describe)
  NO_PACKAGE=1                build without creating the tar.gz
EOF
}

# --- argument parsing ---
TARGET="all"
CONFIG="release"      # lowercase preset name passed to renderers (release|debug)
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        all|renderers|app|scene|video|package|clean) TARGET="$1"; shift ;;
        release|debug) CONFIG="$1"; shift ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

# CMake expects a capitalised build type for the MirageQt app.
if [[ "$CONFIG" == "debug" ]]; then
    CMAKE_CONFIG="Debug"
else
    CMAKE_CONFIG="Release"
fi

# --- platform check ---
[[ "$(uname -s)" == "Linux" ]] || die "This script only supports Linux."

SCENE_SH="$ROOT_DIR/SceneRenderer/scripts/build.sh"
VIDEO_SH="$ROOT_DIR/VideoRenderer/scripts/build.sh"

for s in "$SCENE_SH" "$VIDEO_SH"; do
    [[ -f "$s" ]] || die "missing sub-script: $s"
done

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"

# MirageQt has no sub-build-script; it is built directly with CMake into a
# config-specific directory so a dev build in MirageQt/build is never touched.
MIRAGEQT_DIR="$ROOT_DIR/MirageQt"
APP_BUILD_DIR="${MIRAGEQT_BUILD_DIR:-$MIRAGEQT_DIR/build/$CONFIG}"
PACKAGE_DIR="${PACKAGE_DIR:-$ROOT_DIR/dist}"

# --- per-component build helpers ---
build_scene() {
    step "Building SceneRenderer ($CONFIG)"
    bash "$SCENE_SH" "$CONFIG"
}
build_video() {
    step "Building VideoRenderer ($CONFIG)"
    bash "$VIDEO_SH" "$CONFIG"
}
build_renderers() {
    build_scene
    build_video
}
build_app() {
    step "Building MirageQt ($CMAKE_CONFIG)"
    info "cmake -S $MIRAGEQT_DIR -B $APP_BUILD_DIR (jobs=$JOBS)"
    cmake -S "$MIRAGEQT_DIR" -B "$APP_BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$CMAKE_CONFIG" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build "$APP_BUILD_DIR" --parallel "$JOBS"
    [[ -x "$APP_BUILD_DIR/MirageQt" ]] || die "MirageQt binary not produced at $APP_BUILD_DIR/MirageQt"
    good "MirageQt binary: $APP_BUILD_DIR/MirageQt"
}

# --- clean: remove all sub-project build dirs + dist ---
clean_all() {
    step "Cleaning all build directories"
    bash "$SCENE_SH" clean "$CONFIG" || true
    bash "$VIDEO_SH" clean "$CONFIG" || true
    if [[ -d "$APP_BUILD_DIR" ]]; then
        info "Removing $APP_BUILD_DIR"
        rm -rf "$APP_BUILD_DIR"
    fi
    if [[ -d "$PACKAGE_DIR" ]]; then
        info "Removing $PACKAGE_DIR"
        rm -rf "$PACKAGE_DIR"
    fi
}

# --- artifact lookup helpers ---
first_existing() {
    local p
    for p in "$@"; do
        if [[ -x "$p" ]]; then echo "$p"; return 0; fi
    done
    return 1
}

find_display_lib_dir() {
    # Roots where MirageLinuxDisplay is compiled in-tree.
    local roots=(
        "$ROOT_DIR/SceneRenderer/build/linux-clang-$CONFIG"
        "$ROOT_DIR/SceneRenderer/build"
        "$ROOT_DIR/VideoRenderer/build/$CONFIG"
        "$ROOT_DIR/VideoRenderer/build"
        "$APP_BUILD_DIR"
        "$ROOT_DIR/MirageQt/build"
    )
    local r
    for r in "${roots[@]}"; do
        if [[ -e "$r/MirageLinuxDisplay/src/libmirage_display.so.0" ]]; then
            echo "$r/MirageLinuxDisplay/src"
            return 0
        fi
    done
    return 1
}

# --- package: stage existing artifacts and create the "大包围" tar.gz ---
package() {
    step "Packaging MirageWallpaper ($CONFIG)"

    local scene_bin video_bin app_bin
    scene_bin="$(first_existing \
        "$ROOT_DIR/SceneRenderer/build/linux-clang-$CONFIG/Tools/SceneWallpaper/SceneWallpaper" \
        "$ROOT_DIR/SceneRenderer/build/release/Tools/SceneWallpaper/SceneWallpaper" \
        "$ROOT_DIR/SceneRenderer/build/debug/Tools/SceneWallpaper/SceneWallpaper")" \
        || die "SceneWallpaper binary not found (build it first: $SCENE_SH $CONFIG)"
    video_bin="$(first_existing \
        "$ROOT_DIR/VideoRenderer/build/$CONFIG/Tools/VideoWallpaper/VideoWallpaper" \
        "$ROOT_DIR/VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper" \
        "$ROOT_DIR/VideoRenderer/build/Tools/VideoWallpaper/VideoWallpaper")" \
        || die "VideoWallpaper binary not found (build it first: $VIDEO_SH $CONFIG)"
    app_bin="$APP_BUILD_DIR/MirageQt"
    [[ -x "$app_bin" ]] || die "MirageQt binary not found at $app_bin (build it first: $0 app)"

    local version="${VERSION:-$(git -C "$ROOT_DIR" describe --tags --always --dirty 2>/dev/null || echo 0.1.0)}"
    version="${version//\//-}"
    local arch="$(uname -m)"
    local name="MirageWallpaper-${version}-linux-${arch}"
    local pkg_dir="$PACKAGE_DIR/$name"

    info "package name: $name"
    info "staging into: $pkg_dir"
    rm -rf "$pkg_dir"
    mkdir -p "$pkg_dir"

    info "copying binaries"
    cp -f "$app_bin"   "$pkg_dir/MirageQt"
    cp -f "$scene_bin" "$pkg_dir/SceneWallpaper"
    cp -f "$video_bin" "$pkg_dir/VideoWallpaper"
    chmod +x "$pkg_dir"/MirageQt "$pkg_dir"/SceneWallpaper "$pkg_dir"/VideoWallpaper

    # Renderers link libmirage_display.so.0 at runtime; bundle it beside them
    # and rewrite the RUNPATH to $ORIGIN so the package works outside the repo.
    local lib_dir
    lib_dir="$(find_display_lib_dir)" || true
    if [[ -n "$lib_dir" ]]; then
        info "bundling mirage-display shared library from $lib_dir"
        cp -a "$lib_dir"/libmirage_display.so* "$pkg_dir"/
        if command -v patchelf >/dev/null 2>&1; then
            info "rewriting renderer RUNPATH to \$ORIGIN"
            patchelf --set-rpath '$ORIGIN' \
                "$pkg_dir/SceneWallpaper" "$pkg_dir/VideoWallpaper"
        else
            warn "patchelf not found; renderers keep their build-dir RUNPATH." \
                "Install patchelf and re-run, or launch with LD_LIBRARY_PATH=$pkg_dir."
        fi
    else
        warn "libmirage_display.so.0 not found; the package may not run outside this checkout."
    fi

    info "copying assets/"
    cp -a "$ROOT_DIR/assets" "$pkg_dir/assets"

    info "writing icon and desktop entry"
    cp -f "$ROOT_DIR/MirageQt/Resources/mirage.png" "$pkg_dir/mirageqt.png"
    sed -e "s|@MIRAGEQT_EXECUTABLE@|$pkg_dir/MirageQt|" \
        -e "s|^Icon=.*|Icon=$pkg_dir/mirageqt.png|" \
        "$ROOT_DIR/MirageQt/Resources/mirageqt.desktop.in" > "$pkg_dir/mirageqt.desktop"

    local tarball="$PACKAGE_DIR/$name.tar.gz"
    info "creating $tarball"
    ( cd "$PACKAGE_DIR" && tar -czf "$name.tar.gz" "$name" )
    good "artifact: $tarball"
    du -sh "$tarball" "$pkg_dir" | sed 's/^/  /'
}

# --- dispatch ---
case "$TARGET" in
    scene)     build_scene ;;
    video)     build_video ;;
    renderers) build_renderers ;;
    app)       build_app ;;
    package)   package ;;
    clean)     clean_all ;;
    all)       build_renderers; build_app; [[ "${NO_PACKAGE:-0}" == "1" ]] || package ;;
esac

if [[ "$TARGET" == "all" || "$TARGET" == "app" ]]; then
    step "Build complete"
    good "MirageQt binary: $APP_BUILD_DIR/MirageQt"
    if [[ "$TARGET" == "all" && "${NO_PACKAGE:-0}" != "1" ]]; then
        good "tarball: $PACKAGE_DIR/MirageWallpaper-*-linux-*.tar.gz"
    fi
else
    good "done."
fi
