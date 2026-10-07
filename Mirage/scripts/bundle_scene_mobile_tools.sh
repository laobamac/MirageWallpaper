#!/bin/bash
#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

set -euo pipefail

APP="${1:?用法: bundle_scene_mobile_tools.sh <Mirage.app> <项目根目录> <架构> [签名身份]}"
ROOT="${2:?缺少项目根目录}"
TARGET_ARCH="${3:?缺少目标架构}"
SIGN_IDENTITY="${4:--}"

case "$TARGET_ARCH" in
    arm64|x86_64) ;;
    *) echo "[scene-mobile] 不支持的架构: $TARGET_ARCH" >&2; exit 1 ;;
esac

ETC2_REVISION="39422c1aa2f4889d636db5790af1d0be6ff3a226"
SOURCE="$ROOT/Mirage/build/SceneMobileTools/etc2comp-source"
LICENSE="$SOURCE/LICENSE"
BUILD="$ROOT/Mirage/build/SceneMobileTools/etc2comp-$TARGET_ARCH"
TOOLS="$APP/Contents/Resources/SceneMobileTools"
FFMPEG_PREFIX="$ROOT/Mirage/build/ffmpeg/$TARGET_ARCH"
FFMPEG="$FFMPEG_PREFIX/bin/ffmpeg"
FFMPEG_LICENSES="$FFMPEG_PREFIX/Licenses"

[ -x "$FFMPEG" ] || { echo "[scene-mobile] 缺少 ffmpeg: $FFMPEG（请先运行 scripts/build_ffmpeg.sh $TARGET_ARCH）" >&2; exit 1; }
lipo "$FFMPEG" -verify_arch "$TARGET_ARCH" || {
    echo "[scene-mobile] ffmpeg 架构不匹配 ($TARGET_ARCH): $FFMPEG" >&2
    exit 1
}

if [ -e "$SOURCE" ] && [ ! -d "$SOURCE/.git" ]; then
    echo "[scene-mobile] Etc2Comp 缓存不是有效的 Git 仓库: $SOURCE" >&2
    exit 1
fi

if [ ! -d "$SOURCE/.git" ]; then
    [ "${MIRAGE_ALLOW_NETWORK_FETCH:-0}" = "1" ] || {
        echo "[scene-mobile] 缺少 Etc2Comp 源码缓存。请先设置 MIRAGE_ALLOW_NETWORK_FETCH=1 允许构建脚本联网获取固定版本。" >&2
        exit 1
    }
    echo "[scene-mobile] 获取 Etc2Comp $ETC2_REVISION..."
    mkdir -p "$(dirname "$SOURCE")"
    git clone --depth 1 https://github.com/google/etc2comp.git "$SOURCE" >/dev/null 2>&1
    git -C "$SOURCE" fetch --depth 1 origin "$ETC2_REVISION" >/dev/null 2>&1
    git -C "$SOURCE" checkout --detach "$ETC2_REVISION" >/dev/null
fi

SOURCE_REVISION="$(git -C "$SOURCE" rev-parse HEAD 2>/dev/null || true)"
if [ "$SOURCE_REVISION" != "$ETC2_REVISION" ]; then
    echo "[scene-mobile] 将 Etc2Comp 缓存切换至固定提交 $ETC2_REVISION..."
    if ! git -C "$SOURCE" cat-file -e "$ETC2_REVISION^{commit}" 2>/dev/null; then
        [ "${MIRAGE_ALLOW_NETWORK_FETCH:-0}" = "1" ] || {
            echo "[scene-mobile] 缓存中没有固定版本 $ETC2_REVISION。请设置 MIRAGE_ALLOW_NETWORK_FETCH=1 允许联网获取。" >&2
            exit 1
        }
        git -C "$SOURCE" fetch --depth 1 origin "$ETC2_REVISION" >/dev/null 2>&1
    fi
    git -C "$SOURCE" checkout --detach "$ETC2_REVISION" >/dev/null
fi

[ "$(git -C "$SOURCE" rev-parse HEAD 2>/dev/null || true)" = "$ETC2_REVISION" ] || {
    echo "[scene-mobile] 无法校验 Etc2Comp 固定提交" >&2
    exit 1
}
PATCH="$ROOT/Mirage/scripts/patches/etc2comp-nan-clamp.patch"
# NaN-to-integer conversion differs across CPUs and can create enormous ETC2
# color-search ranges. Clamp invalid RGB values before the encoder quantizes them.
if git -C "$SOURCE" apply --unidiff-zero --ignore-space-change --check "$PATCH" 2>/dev/null; then
    git -C "$SOURCE" apply --unidiff-zero --ignore-space-change "$PATCH"
elif ! git -C "$SOURCE" apply --unidiff-zero --ignore-space-change --reverse --check "$PATCH" 2>/dev/null; then
    echo "[scene-mobile] Etc2Comp 源码与颜色边界修复不匹配" >&2
    exit 1
fi
[ -f "$SOURCE/CMakeLists.txt" ] || { echo "[scene-mobile] Etc2Comp 源码不完整" >&2; exit 1; }
[ -f "$LICENSE" ] || { echo "[scene-mobile] Etc2Comp 许可证缺失" >&2; exit 1; }
for license in FFmpeg-LICENSE.md FFmpeg-LGPL-2.1.txt FFmpeg-BUILD.txt; do
    [ -f "$FFMPEG_LICENSES/$license" ] || { echo "[scene-mobile] FFmpeg 许可证文件缺失: $FFMPEG_LICENSES/$license" >&2; exit 1; }
done

echo "[scene-mobile] 编译 EtcTool ($TARGET_ARCH)..."
cmake --fresh -S "$SOURCE" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_OSX_ARCHITECTURES="$TARGET_ARCH" >/dev/null
cmake --build "$BUILD" --target EtcTool --parallel >/dev/null
ETC_TOOL="$BUILD/EtcTool/EtcTool"
[ -x "$ETC_TOOL" ] || { echo "[scene-mobile] EtcTool 构建失败" >&2; exit 1; }
file "$ETC_TOOL" | grep -q "$TARGET_ARCH" || {
    echo "[scene-mobile] EtcTool 架构不匹配 ($TARGET_ARCH): $ETC_TOOL" >&2
    exit 1
}

rm -rf "$TOOLS"
mkdir -p "$TOOLS"
cp -f "$ETC_TOOL" "$TOOLS/EtcTool"
cp -f "$FFMPEG" "$TOOLS/ffmpeg"
cp -f "$LICENSE" "$TOOLS/Etc2Comp-LICENSE.txt"
cp -f "$PATCH" "$TOOLS/Etc2Comp-PATCH.diff"
cp -f "$FFMPEG_LICENSES/FFmpeg-LICENSE.md" "$TOOLS/FFmpeg-LICENSE.md"
cp -f "$FFMPEG_LICENSES/FFmpeg-LGPL-2.1.txt" "$TOOLS/FFmpeg-LGPL-2.1.txt"
cp -f "$FFMPEG_LICENSES/FFmpeg-BUILD.txt" "$TOOLS/FFmpeg-BUILD.txt"
chmod +x "$TOOLS/EtcTool" "$TOOLS/ffmpeg"

SIGN_ARGS=(--timestamp=none)
if [ "$SIGN_IDENTITY" != "-" ]; then
    SIGN_ARGS=(--timestamp --options runtime)
fi

codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$TOOLS/ffmpeg"
codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$TOOLS/EtcTool"
codesign --force "${SIGN_ARGS[@]}" \
    --entitlements "$ROOT/Mirage/Mirage Wallpaper/Mirage_Wallpaper.entitlements" \
    --sign "$SIGN_IDENTITY" "$APP"

echo "[scene-mobile] 已内嵌 ffmpeg 与 EtcTool"
