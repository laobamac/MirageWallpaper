#!/usr/bin/env bash
# Run logic-only Swift tests without launching the Mirage app lifecycle.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIRAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT="$MIRAGE_DIR/Mirage Wallpaper.xcodeproj"
SCHEME="Mirage Wallpaper"
EXPECTED_TEST_COUNT="${MIRAGE_SWIFT_TEST_COUNT:-7}"

for tool in xcodebuild xcrun python3; do
    command -v "$tool" >/dev/null || {
        printf 'ERROR: required tool not found: %s\n' "$tool" >&2
        exit 1
    }
done

architecture="$(uname -m)"
case "$architecture" in
    arm64|x86_64) ;;
    *)
        printf 'ERROR: unsupported architecture: %s\n' "$architecture" >&2
        exit 1
        ;;
esac

sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
sdk_name="$(basename "$sdk_path" .sdk)"
derived_data="${MIRAGE_SWIFT_TEST_BUILD_DIR:-$MIRAGE_DIR/build/SwiftTests-${sdk_name}-${architecture}}"
result_root="$(mktemp -d "${TMPDIR:-/tmp}/mirage-swift-tests.XXXXXX")"
result_bundle="$result_root/MirageSwiftTests.xcresult"

cleanup() {
    status=$?
    if [[ "$status" -eq 0 ]]; then
        rm -rf "$result_root"
    else
        printf 'Swift test result retained at %s\n' "$result_bundle" >&2
    fi
}
trap cleanup EXIT

printf 'Running Mirage logic-only Swift tests\n'
printf '  architecture: %s\n' "$architecture"
printf '  SDK:          %s\n' "$sdk_path"
printf '  Xcode:        %s\n' "$(xcodebuild -version | head -1)"
printf '  build dir:    %s\n' "$derived_data"

xcodebuild test -quiet \
    -project "$PROJECT" \
    -scheme "$SCHEME" \
    -destination "platform=macOS,arch=$architecture" \
    -only-testing:'Mirage WallpaperTests' \
    -derivedDataPath "$derived_data" \
    -clonedSourcePackagesDirPath "$derived_data/SourcePackages" \
    -resultBundlePath "$result_bundle" \
    CODE_SIGNING_ALLOWED=NO \
    COMPILER_INDEX_STORE_ENABLE=NO \
    ONLY_ACTIVE_ARCH=YES \
    ARCHS="$architecture"

xcrun xcresulttool get test-results tests \
    --compact \
    --path "$result_bundle" \
    | python3 "$SCRIPT_DIR/validate_test_results.py" "$EXPECTED_TEST_COUNT"
