#!/bin/bash
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
LIBS="$TOOLS/lib"
if [ "$TARGET_ARCH" = "x86_64" ]; then
    BREW_PREFIX="/usr/local"
else
    BREW_PREFIX="/opt/homebrew"
fi
FFMPEG="$BREW_PREFIX/opt/ffmpeg/bin/ffmpeg"
FFMPEG_ROOT="$(cd "$(dirname "$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$FFMPEG")")/.." 2>/dev/null && pwd || true)"
FFMPEG_LICENSE="$FFMPEG_ROOT/LICENSE.md"
FFMPEG_COPYING="$FFMPEG_ROOT/COPYING.GPLv3"

[ -x "$FFMPEG" ] || { echo "[scene-mobile] 缺少 ffmpeg: $FFMPEG" >&2; exit 1; }
file "$FFMPEG" | grep -q "$TARGET_ARCH" || {
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
[ -f "$SOURCE/CMakeLists.txt" ] || { echo "[scene-mobile] Etc2Comp 源码不完整" >&2; exit 1; }
[ -f "$LICENSE" ] || { echo "[scene-mobile] Etc2Comp 许可证缺失" >&2; exit 1; }
[ -f "$FFMPEG_LICENSE" ] || { echo "[scene-mobile] FFmpeg 许可证说明缺失: $FFMPEG_LICENSE" >&2; exit 1; }
[ -f "$FFMPEG_COPYING" ] || { echo "[scene-mobile] FFmpeg GPLv3 许可证缺失: $FFMPEG_COPYING" >&2; exit 1; }

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
mkdir -p "$TOOLS" "$LIBS"
cp -f "$ETC_TOOL" "$TOOLS/EtcTool"
cp -f "$FFMPEG" "$TOOLS/ffmpeg"
cp -f "$LICENSE" "$TOOLS/Etc2Comp-LICENSE.txt"
cp -f "$FFMPEG_LICENSE" "$TOOLS/FFmpeg-LICENSE.md"
cp -f "$FFMPEG_COPYING" "$TOOLS/FFmpeg-COPYING.GPLv3.txt"
cat > "$TOOLS/FFmpeg-SOURCE.txt" <<'EOF'
FFmpeg
Upstream project: https://ffmpeg.org/
Source downloads: https://ffmpeg.org/download.html
Homebrew formula: https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/f/ffmpeg.rb

The bundled executable reports its exact version and build configuration when
invoked with `ffmpeg -version`. The Homebrew build used by Mirage enables GPL
and version 3 components, so the bundled executable is distributed under GPLv3.
EOF
chmod +x "$TOOLS/EtcTool" "$TOOLS/ffmpeg"

SIGN_ARGS=(--timestamp=none)
if [ "$SIGN_IDENTITY" != "-" ]; then
    SIGN_ARGS=(--timestamp --options runtime)
fi

is_bundleable() {
    case "$1" in
        /usr/lib/*|/System/*|@rpath/*|@loader_path/*|@executable_path/*) return 1 ;;
        *) return 0 ;;
    esac
}

resolve() {
    if [ -f "$1" ]; then
        python3 -c "import os,sys;print(os.path.realpath(sys.argv[1]))" "$1"
    else
        echo "$1"
    fi
}

COPIED_LIST="$(mktemp -t mirage-scene-mobile-libs)"
trap 'rm -f "$COPIED_LIST"' EXIT

is_copied() { grep -qxF "$1" "$COPIED_LIST" 2>/dev/null; }
mark_copied() { echo "$1" >> "$COPIED_LIST"; }

collect_deps() {
    local target="$1"
    local deps
    deps=$(otool -L "$target" | tail -n +2 | awk '{print $1}')
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        is_bundleable "$dep" || continue
        local real base
        real=$(resolve "$dep")
        base=$(basename "$real")
        if ! is_copied "$base"; then
            [ -f "$real" ] || { echo "[scene-mobile] 找不到依赖: $dep" >&2; exit 1; }
            file "$real" | grep -q "$TARGET_ARCH" || {
                echo "[scene-mobile] ffmpeg 依赖架构不匹配 ($TARGET_ARCH): $real" >&2
                exit 1
            }
            mark_copied "$base"
            # Always replace a same-named library. The arm64 and x86_64 builds
            # share DerivedData, so an existing file may belong to the previous
            # architecture even though its basename is identical.
            cp -f "$real" "$LIBS/$base"
            chmod u+w "$LIBS/$base"
            collect_deps "$LIBS/$base"
        fi
    done <<< "$deps"
}

collect_deps "$TOOLS/ffmpeg"

retarget_lib() {
    local lib="$1"
    local base deps
    base=$(basename "$lib")
    install_name_tool -id "@rpath/$base" "$lib" 2>/dev/null || true
    deps=$(otool -L "$lib" | tail -n +2 | awk '{print $1}')
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        is_bundleable "$dep" || continue
        local dep_base
        dep_base=$(basename "$(resolve "$dep")")
        if [ -f "$LIBS/$dep_base" ]; then
            install_name_tool -change "$dep" "@rpath/$dep_base" "$lib"
        fi
    done <<< "$deps"
}

while IFS= read -r base; do
    [ -f "$LIBS/$base" ] || continue
    retarget_lib "$LIBS/$base"
done < "$COPIED_LIST"

deps=$(otool -L "$TOOLS/ffmpeg" | tail -n +2 | awk '{print $1}')
while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    is_bundleable "$dep" || continue
    dep_base=$(basename "$(resolve "$dep")")
    if [ -f "$LIBS/$dep_base" ]; then
        install_name_tool -change "$dep" "@rpath/$dep_base" "$TOOLS/ffmpeg"
    fi
done <<< "$deps"
install_name_tool -add_rpath "@loader_path/lib" "$TOOLS/ffmpeg" 2>/dev/null || true

while IFS= read -r base; do
    [ -f "$LIBS/$base" ] || continue
    codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$LIBS/$base"
done < "$COPIED_LIST"
codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$TOOLS/ffmpeg"
codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$TOOLS/EtcTool"
codesign --force "${SIGN_ARGS[@]}" \
    --entitlements "$ROOT/Mirage/Mirage Wallpaper/Mirage_Wallpaper.entitlements" \
    --sign "$SIGN_IDENTITY" "$APP"

echo "[scene-mobile] 已内嵌 ffmpeg、EtcTool 与 $(wc -l < "$COPIED_LIST" | tr -d ' ') 个依赖"
