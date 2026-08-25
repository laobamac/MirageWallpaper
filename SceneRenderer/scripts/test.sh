#!/usr/bin/env bash
#
# Build and run the SceneRenderer regression suite in an isolated CMake tree.
# The normal renderer build cache is intentionally left untouched so this gate
# can run before and after refactors without changing packaged artifacts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$PROJECT_DIR/.." && pwd)"

source "$ROOT_DIR/scripts/preset.sh"

[[ "$(uname -s)" == "Darwin" ]] || {
    printf 'ERROR: SceneRenderer regression tests currently require macOS.\n' >&2
    exit 1
}

for tool in brew cmake ninja ctest; do
    command -v "$tool" >/dev/null || {
        printf 'ERROR: required tool not found: %s\n' "$tool" >&2
        exit 1
    }
done

PRESET="${BUILD_PRESET:-$(scene_preset release)}"
case "$PRESET" in
    macos-clang-release|macos-clang-debug|macos-arm64-clang-release|macos-arm64-clang-debug) ;;
    *)
        printf 'ERROR: unsupported BUILD_PRESET: %s\n' "$PRESET" >&2
        exit 1
        ;;
esac

LLVM_PREFIX="$(brew --prefix llvm)"
CLANG_BIN="$LLVM_PREFIX/bin/clang"
CLANGXX_BIN="$LLVM_PREFIX/bin/clang++"
[[ -x "$CLANG_BIN" && -x "$CLANGXX_BIN" ]] || {
    printf 'ERROR: Homebrew LLVM is not installed at %s.\n' "$LLVM_PREFIX" >&2
    exit 1
}

command -v xcrun >/dev/null || {
    printf 'ERROR: xcrun is required to select a macOS SDK.\n' >&2
    exit 1
}

# Resolve the SDK through the selected Xcode. CMake otherwise may discover the
# separately installed Command Line Tools SDK, producing an unauditable mix of
# headers and frameworks from different toolchains.
SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
[[ -d "$SDK_PATH" ]] || {
    printf 'ERROR: selected macOS SDK does not exist: %s\n' "$SDK_PATH" >&2
    exit 1
}
SDK_NAME="$(basename "$SDK_PATH" .sdk)"

# Keep stable and beta SDK caches physically separate. This prevents CMake's
# cached find_library results from carrying framework paths across SDKs.
BUILD_DIR="${SCENERENDERER_TEST_BUILD_DIR:-$PROJECT_DIR/build/${PRESET}-${SDK_NAME}-tests}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || printf '8')}"

cd "$PROJECT_DIR"

printf 'Configuring SceneRenderer regression tests\n'
printf '  preset:    %s\n' "$PRESET"
printf '  SDK:       %s\n' "$SDK_PATH"
printf '  compiler:  %s\n' "$($CLANGXX_BIN --version | head -1)"
printf '  build dir: %s\n' "$BUILD_DIR"

cmake --preset "$PRESET" \
    -B "$BUILD_DIR" \
    -DCMAKE_C_COMPILER="$CLANG_BIN" \
    -DCMAKE_CXX_COMPILER="$CLANGXX_BIN" \
    -DCMAKE_OSX_SYSROOT="$SDK_PATH" \
    -DSCENERENDERER_BUILD_TESTS=ON \
    -DSCENERENDERER_BUILD_VIEWER=OFF \
    -DSCENERENDERER_BUILD_WALLPAPER_HOST=OFF

TEST_TARGETS=(
    SceneRendererTextGeometryTests
    SceneRendererLayerVisibilityTests
    SceneRendererRuntimeCompatibilityTests
    SceneRendererScriptCompatibilityTests
    SceneRendererPuppetAlphaTests
)

cmake --build "$BUILD_DIR" \
    --parallel "$JOBS" \
    --target "${TEST_TARGETS[@]}"

test_count="$(ctest --test-dir "$BUILD_DIR" -N | awk '/Total Tests:/ { print $3 }')"
if [[ "$test_count" != "${#TEST_TARGETS[@]}" ]]; then
    printf 'ERROR: expected %d tests, discovered %s.\n' \
        "${#TEST_TARGETS[@]}" "${test_count:-0}" >&2
    exit 1
fi

ctest --test-dir "$BUILD_DIR" \
    --output-on-failure \
    --no-tests=error
