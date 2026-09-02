#!/bin/bash
set -euo pipefail

CONFIG="${1:-Release}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$PROJ_DIR/.." && pwd)"
BUILD_DIR="$PROJ_DIR/build"
PROJECT="$PROJ_DIR/Mirage Wallpaper.xcodeproj"
SCHEME="Mirage Wallpaper"
TARGET_ARCH="${MIRAGE_ARCH:-$(uname -m)}"
STEAM_API_KEY="${MIRAGE_STEAM_WEB_API_KEY:-}"
GIT_COMMIT="${MIRAGE_GIT_COMMIT:-$(git -C "$ROOT" rev-parse HEAD)}"
BUILD_NUMBER="${MIRAGE_BUILD_NUMBER:-$(git -C "$ROOT" rev-list --count HEAD)}"
LOCAL_SECRET="$ROOT/.secrets/steam_web_api_key"
TEMP_XCCONFIG=""
SIGN_IDENTITY="${MIRAGE_SIGN_IDENTITY:--}"
DEVELOPMENT_TEAM="${MIRAGE_DEVELOPMENT_TEAM:-}"
NOTARY_PROFILE="${MIRAGE_NOTARY_PROFILE:-}"
NOTARY_TEMP_DIR=""

case "$TARGET_ARCH" in
    arm64|x86_64) ;;
    *) echo "[build] 不支持的更新架构: $TARGET_ARCH" >&2; exit 1 ;;
esac

[[ "$GIT_COMMIT" =~ ^[A-Fa-f0-9]{40}$ ]] || {
    echo "[build] MIRAGE_GIT_COMMIT 必须是完整 Git commit SHA" >&2
    exit 1
}
[[ "$BUILD_NUMBER" =~ ^[1-9][0-9]*$ ]] || {
    echo "[build] MIRAGE_BUILD_NUMBER 必须是正整数" >&2
    exit 1
}

if [ -z "$STEAM_API_KEY" ] && [ -f "$LOCAL_SECRET" ]; then
    IFS= read -r STEAM_API_KEY < "$LOCAL_SECRET"
fi

XCCONFIG_ARGS=()
TEMP_XCCONFIG="$(mktemp -t mirage-build-settings)"
chmod 600 "$TEMP_XCCONFIG"
printf 'CURRENT_PROJECT_VERSION = %s\n' "$BUILD_NUMBER" >> "$TEMP_XCCONFIG"
printf 'MIRAGE_GIT_COMMIT = %s\n' "$GIT_COMMIT" >> "$TEMP_XCCONFIG"
printf 'MIRAGE_UPDATE_ARCH = %s\n' "$TARGET_ARCH" >> "$TEMP_XCCONFIG"
if [ "$SIGN_IDENTITY" != "-" ]; then
    [ -n "$DEVELOPMENT_TEAM" ] || { echo "[build] 正式签名需要 MIRAGE_DEVELOPMENT_TEAM" >&2; exit 1; }
    printf 'DEVELOPMENT_TEAM = %s\n' "$DEVELOPMENT_TEAM" >> "$TEMP_XCCONFIG"
    printf 'CODE_SIGN_IDENTITY = %s\n' "$SIGN_IDENTITY" >> "$TEMP_XCCONFIG"
    printf 'ENABLE_HARDENED_RUNTIME = YES\n' >> "$TEMP_XCCONFIG"
fi
XCCONFIG_ARGS=(-xcconfig "$TEMP_XCCONFIG")
if [ -n "$STEAM_API_KEY" ]; then
    [[ "$STEAM_API_KEY" =~ ^[A-Fa-f0-9]{32}$ ]] || {
        echo "[build] Steam Web API Key 必须是 32 位十六进制字符" >&2
        exit 1
    }
    printf 'MIRAGE_STEAM_WEB_API_KEY = %s\n' "$STEAM_API_KEY" >> "$TEMP_XCCONFIG"
else
    echo "[build] 未提供内置 Steam Web API Key；App 仍可构建，用户需在设置中填写自己的 Key" >&2
fi

cleanup() {
    [ -z "$TEMP_XCCONFIG" ] || rm -f "$TEMP_XCCONFIG"
    [ -z "$NOTARY_TEMP_DIR" ] || rm -rf "$NOTARY_TEMP_DIR"
}
trap cleanup EXIT

CODE_SIGNING_REQUIRED=NO
if [ "$SIGN_IDENTITY" != "-" ]; then
    CODE_SIGNING_REQUIRED=YES
fi

APP="$BUILD_DIR/DD/Build/Products/$CONFIG/Mirage Wallpaper.app"

if [ -d "$APP/Contents/Resources/Renderers" ]; then
    echo "[build] 清理旧构建塞进产物里的渲染器残留..."
    rm -rf "$APP"
fi

mkdir -p "$BUILD_DIR"
BUILD_LOG="$BUILD_DIR/xcodebuild-$CONFIG.log"
: > "$BUILD_LOG"
chmod 600 "$BUILD_LOG"

echo "[build] 编译 ($CONFIG)..."
if ! xcodebuild "${XCCONFIG_ARGS[@]}" -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIG" \
    -destination 'platform=macOS' \
    -derivedDataPath "$BUILD_DIR/DD" \
    ARCHS="$TARGET_ARCH" ONLY_ACTIVE_ARCH=YES \
    CODE_SIGN_IDENTITY="$SIGN_IDENTITY" CODE_SIGNING_REQUIRED="$CODE_SIGNING_REQUIRED" CODE_SIGNING_ALLOWED=YES \
    build > "$BUILD_LOG" 2>&1; then
    echo "[build] 编译失败，错误摘要:" >&2
    { grep -nE "error:|error extracting|not signed at all|In subcomponent:|failed with a nonzero|The following build commands failed" \
        "$BUILD_LOG" | tail -20 >&2; } || true
    echo "[build] 完整日志: $BUILD_LOG" >&2
    exit 1
fi
tail -3 "$BUILD_LOG"

[ -d "$APP" ] || { echo "[build] 未找到产物: $APP" >&2; exit 1; }

OUT="$PROJ_DIR/dist"
mkdir -p "$OUT"
rm -rf "$OUT/Mirage.app"
ditto "$APP" "$OUT/Mirage.app"

echo "[build] 内嵌渲染器与依赖..."
bash "$HERE/bundle_renderers.sh" "$OUT/Mirage.app" "$ROOT" "$SIGN_IDENTITY"

codesign --verify --deep --strict --verbose=2 "$OUT/Mirage.app"

if [ "$SIGN_IDENTITY" != "-" ] && [ -n "$NOTARY_PROFILE" ]; then
    NOTARY_TEMP_DIR="$(mktemp -d -t mirage-notary)"
    ditto -c -k --keepParent "$OUT/Mirage.app" "$NOTARY_TEMP_DIR/Mirage.zip"
    xcrun notarytool submit "$NOTARY_TEMP_DIR/Mirage.zip" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT/Mirage.app"
    xcrun stapler validate "$OUT/Mirage.app"
fi

echo "[build] 完成  产物: $OUT/Mirage.app"
