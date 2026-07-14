#!/usr/bin/env bash
#
# SceneRenderer — macOS build helper.
#
# Wraps the CMake presets (macos-clang-release / macos-clang-debug) with
# prerequisite checks and a default configure → build → report flow.
#
# macOS-only by design:
#   * MoltenVK is the Vulkan ICD, wavsen uses Core Audio.
#   * Mach-O @loader_path rpath (no ELF $ORIGIN), system ld (no LLD).
#   * No conda / Mesa / Wayland / X11 tooling — those are the
#     upstream Linux build's concerns, not ours.
#
# Usage:
#   scripts/build.sh                # release build (default): configure + build + report
#   scripts/build.sh debug          # debug build
#   scripts/build.sh configure      # configure only (no compile)
#   scripts/build.sh build          # build only (configures first if needed)
#   scripts/build.sh clean          # wipe build/<preset>
#   scripts/build.sh -h|--help
#
# Environment overrides:
#   BUILD_PRESET   macos-clang-release | macos-clang-debug
#   KEEP_GOING=1   keep building past errors (cmake --keep-going)
#   JOBS=N         parallel jobs (default: sysctl -n hw.logicalcpu)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$PROJECT_DIR/.." && pwd)"

# Shared CMake preset naming convention.
source "$ROOT_DIR/scripts/preset.sh"

cd "$PROJECT_DIR"

# --- terminal colors (disabled when not a TTY) ---
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
SceneRenderer macOS build helper.

Usage:
  scripts/build.sh                configure + build + report (release, default)
  scripts/build.sh debug          configure + build + report (debug)
  scripts/build.sh configure      configure only
  scripts/build.sh build          build only (configures first if needed)
  scripts/build.sh clean          wipe build/<preset>
  scripts/build.sh -h|--help      show this help

Environment:
  BUILD_PRESET   macos-clang-release | macos-clang-debug
  KEEP_GOING=1   keep building past errors
  JOBS=N         parallel jobs (default: hw.logicalcpu)
EOF
}

# --- parse args ---
ACTION="all"
POSITIONAL_PRESET=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        configure|build|all|clean) ACTION="$1"; shift ;;
        release|debug)
            POSITIONAL_PRESET="$(scene_preset "$1")"
            shift ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

# --- preset resolution ---
DEFAULT_PRESET="$(scene_preset release)"
PRESET="${BUILD_PRESET:-${POSITIONAL_PRESET:-$DEFAULT_PRESET}}"
case "$PRESET" in
    macos-clang-release|macos-clang-debug|macos-arm64-clang-release|macos-arm64-clang-debug) ;;
    *) die "unknown preset: $PRESET (expected macos-clang-release, macos-clang-debug, macos-arm64-clang-release, or macos-arm64-clang-debug)" ;;
esac
BUILD_DIR="$PROJECT_DIR/build/$PRESET"

# --- platform + core tools ---
[[ "$(uname -s)" == "Darwin" ]] || die "this script is macOS-only."
command -v brew       >/dev/null || die "Homebrew not found. Install from https://brew.sh"
command -v cmake      >/dev/null || die "cmake not found. brew install cmake"
command -v ninja      >/dev/null || die "ninja not found. brew install ninja"
command -v pkg-config >/dev/null || die "pkg-config not found. brew install pkg-config"

BREW_PREFIX="$(brew --prefix)"
[[ -n "$BREW_PREFIX" ]] || die "brew --prefix returned empty."

# --- prerequisite Homebrew formulas (build + runtime) ---
# Each entry: "formula|description"
REQUIRED_FORMULAS=(
    "llvm|Clang 22+ compiler (C++20 modules)"
    "molten-vk|MoltenVK Vulkan ICD"
    "vulkan-loader|libvulkan loader"
    "vulkan-headers|Vulkan headers"
    "glslang|glslangValidator shader compiler"
    "glfw|GLFW (SceneViewer window)"
    "freetype|FreeType (text rasterization)"
    "fontconfig|Fontconfig (font discovery)"
    "lz4|liblz4 (.pkg decompression)"
    "ffmpeg|ffmpeg (wavsen video decode)"
)

# One brew call for the full installed set, then membership-check.
INSTALLED_FORMULAS="$(brew list --formula -1 2>/dev/null || true)"
missing=()
for entry in "${REQUIRED_FORMULAS[@]}"; do
    f="${entry%%|*}"; what="${entry##*|}"
    if ! grep -qxF "$f" <<<"$INSTALLED_FORMULAS"; then
        missing+=("$f")
        warn "missing Homebrew formula: $f ($what)"
    fi
done
if [[ ${#missing[@]} -gt 0 ]]; then
    printf '\nInstall missing dependencies:\n  brew install %s\n\n' "${missing[*]}"
    die "missing Homebrew dependencies (see above)."
fi
command -v glslangValidator >/dev/null || die "glslangValidator not found. Run: brew install glslang"

# --- compiler: always use Homebrew LLVM. ---
# The preset pins /usr/local/opt/llvm/bin/clang (Intel Mac path). On Apple
# Silicon that path is absent, so pass -D to redirect to the actual Homebrew
# LLVM prefix. On Intel this is the same path, so it's a no-op override.
LLVM_PREFIX="$(brew --prefix llvm)"
CLANG_BIN="$LLVM_PREFIX/bin/clang"
CLANGXX_BIN="$LLVM_PREFIX/bin/clang++"
[[ -x "$CLANG_BIN" && -x "$CLANGXX_BIN" ]] || die "Homebrew clang not at $LLVM_PREFIX/bin (brew install llvm)"

# --- MoltenVK ICD (runtime Vulkan device discovery) ---
ICD_JSON="$BREW_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json"
[[ -f "$ICD_JSON" ]] || warn "MoltenVK ICD json not found at $ICD_JSON; runtime Vulkan may fail to pick MoltenVK."

# --- parallel jobs ---
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)}"

# --- actions ---
do_clean() {
    if [[ ! -d "$BUILD_DIR" ]]; then
        info "nothing to clean ($BUILD_DIR absent)"
        return
    fi
    info "cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
}

do_configure() {
    info "configuring preset: $PRESET"
    info "  project:  $PROJECT_DIR"
    info "  build dir:$BUILD_DIR"
    info "  compiler: $CLANG_BIN"
    cmake --preset "$PRESET" \
        -DCMAKE_C_COMPILER="$CLANG_BIN" \
        -DCMAKE_CXX_COMPILER="$CLANGXX_BIN"
}

do_build() {
    [[ -f "$BUILD_DIR/CMakeCache.txt" ]] || do_configure
    info "building preset: $PRESET (jobs=$JOBS)"
    local build_args=(--build "$BUILD_DIR" --parallel "$JOBS")
    if [[ "${KEEP_GOING:-0}" == "1" ]]; then
        build_args+=(--keep-going)
    fi
    cmake "${build_args[@]}"
}

report() {
    info "build artifacts:"
    local bin found=0
    for bin in \
        "$BUILD_DIR/Tools/SceneViewer/SceneViewer" \
        "$BUILD_DIR/Tools/SceneWallpaper/SceneWallpaper"; do
        if [[ -x "$bin" ]]; then
            printf '  %s\n' "$bin"
            found=1
        fi
    done
    if [[ $found -eq 0 ]]; then
        # Fallback: locate by name in case CMake's output layout differs.
        while IFS= read -r bin; do
            printf '  %s\n' "$bin"
            found=1
        done < <(find "$BUILD_DIR" -type f \( -name SceneViewer -o -name SceneWallpaper \) \
                    ! -path '*/CMakeFiles/*' 2>/dev/null)
    fi
    if [[ $found -eq 0 ]]; then
        warn "no SceneViewer/SceneWallpaper binaries found under $BUILD_DIR (build failed or not run)."
    else
        printf '\nRun e.g.:\n  %s/Tools/SceneViewer/SceneViewer <path/to/scene.pkg>\n' "$BUILD_DIR"
    fi
}

case "$ACTION" in
    clean)     do_clean ;;
    configure) do_configure ;;
    build)     do_build; report ;;
    all)       do_configure; do_build; report ;;
esac

good "done."
