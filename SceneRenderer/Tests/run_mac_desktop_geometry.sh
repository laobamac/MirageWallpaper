#!/usr/bin/env bash
set -euo pipefail
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mirage-geometry.XXXXXX")"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
xcrun clang++ -std=c++20 -O2 -fno-objc-arc \
    -Wno-deprecated-declarations -Wno-deprecated-enum-enum-conversion \
    "$TEST_DIR/MacDesktopGeometryRegression.mm" \
    -framework Cocoa -framework CoreGraphics -framework Metal \
    -framework MetalFX -framework QuartzCore \
    -o "$TEST_BUILD_DIR/MacDesktopGeometryRegression"
"$TEST_BUILD_DIR/MacDesktopGeometryRegression"
