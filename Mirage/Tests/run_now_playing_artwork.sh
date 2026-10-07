#!/usr/bin/env bash
#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

set -euo pipefail
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mirage-artwork.XXXXXX")"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
xcrun swiftc -parse-as-library -O \
    "$TEST_DIR/../Mirage Wallpaper/Services/NowPlayingService.swift" \
    "$TEST_DIR/NowPlayingArtworkRegression.swift" \
    -o "$TEST_BUILD_DIR/NowPlayingArtworkRegression"
"$TEST_BUILD_DIR/NowPlayingArtworkRegression"
