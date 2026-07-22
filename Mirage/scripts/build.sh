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
}
trap cleanup EXIT

echo "[build] 编译 ($CONFIG)..."
xcodebuild "${XCCONFIG_ARGS[@]}" -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIG" \
    -destination 'platform=macOS' \
    -derivedDataPath "$BUILD_DIR/DD" \
    ARCHS="$TARGET_ARCH" ONLY_ACTIVE_ARCH=YES \
    CODE_SIGN_IDENTITY="-" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=YES \
    build | tail -3

APP="$BUILD_DIR/DD/Build/Products/$CONFIG/Mirage Wallpaper.app"
[ -d "$APP" ] || { echo "[build] 未找到产物: $APP" >&2; exit 1; }

echo "[build] 内嵌渲染器与依赖..."
bash "$HERE/bundle_renderers.sh" "$APP" "$ROOT"

OUT="$PROJ_DIR/dist"
mkdir -p "$OUT"
rm -rf "$OUT/Mirage.app"
cp -R "$APP" "$OUT/Mirage.app"
codesign --force --deep --sign - "$OUT/Mirage.app" 2>/dev/null || true

echo "[build] 完成  产物: $OUT/Mirage.app"
