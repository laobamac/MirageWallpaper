#!/bin/bash
set -euo pipefail

APP="${1:?用法: bundle_renderers.sh <Mirage.app> [SimpleRenderer根]}"
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# 共享的 CMake preset 命名约定。
source "$ROOT/scripts/preset.sh"

CONTENTS="$APP/Contents"
FRAMEWORKS="$CONTENTS/Frameworks"
RESOURCES="$CONTENTS/Resources"
RENDERERS="$RESOURCES/Renderers"
VK_ICD_DIR="$RENDERERS/vulkan/icd.d"

SCENE_PRESET="$(scene_preset release)"
SCENE_BIN="$ROOT/SceneRenderer/build/$SCENE_PRESET/Tools/SceneWallpaper/SceneWallpaper"
WEB_BIN="$ROOT/WebRenderer/build/release/Tools/WebWallpaper/WebWallpaper"
VIDEO_BIN="$ROOT/VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper"
ASSETS_DIR="$ROOT/assets"

BREW_PREFIX="$(brew --prefix)"
MOLTENVK="$BREW_PREFIX/opt/molten-vk/lib/libMoltenVK.dylib"

echo "[bundle] App:  $APP"
echo "[bundle] Root: $ROOT"

for f in "$SCENE_BIN" "$WEB_BIN" "$VIDEO_BIN"; do
    [ -f "$f" ] || { echo "[bundle] 缺少渲染器: $f" >&2; exit 1; }
done
[ -d "$ASSETS_DIR" ] || { echo "[bundle] 缺少 assets 目录: $ASSETS_DIR" >&2; exit 1; }
[ -f "$MOLTENVK" ] || { echo "[bundle] 缺少 MoltenVK: $MOLTENVK" >&2; exit 1; }

mkdir -p "$FRAMEWORKS" "$RENDERERS" "$VK_ICD_DIR"

cp -f "$SCENE_BIN" "$RENDERERS/SceneWallpaper"
cp -f "$WEB_BIN"   "$RENDERERS/WebWallpaper"
cp -f "$VIDEO_BIN" "$RENDERERS/VideoWallpaper"
chmod +x "$RENDERERS"/*

is_bundleable() {
    case "$1" in
        /usr/lib/*|/System/*) return 1 ;;
        @rpath/*|@loader_path/*|@executable_path/*) return 1 ;;
        *) return 0 ;;
    esac
}

resolve() {
    local p="$1"
    if [ -f "$p" ]; then
        python3 -c "import os,sys;print(os.path.realpath(sys.argv[1]))" "$p"
    else
        echo "$p"
    fi
}

# bash 3.2 没有关联数组。
COPIED_LIST="$FRAMEWORKS/.copied"
: > "$COPIED_LIST"

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
            if [ -f "$real" ]; then
                mark_copied "$base"
                cp -f "$real" "$FRAMEWORKS/$base"
                chmod u+w "$FRAMEWORKS/$base"
                collect_deps "$FRAMEWORKS/$base"
            else
                echo "[bundle] 警告: 找不到依赖 $dep (real=$real)" >&2
            fi
        fi
    done <<< "$deps"
}

echo "[bundle] 收集场景引擎依赖..."
collect_deps "$RENDERERS/SceneWallpaper"

MVK_BASE=$(basename "$MOLTENVK")
if ! is_copied "$MVK_BASE"; then
    cp -f "$(resolve "$MOLTENVK")" "$FRAMEWORKS/$MVK_BASE"
    chmod u+w "$FRAMEWORKS/$MVK_BASE"
    mark_copied "$MVK_BASE"
    collect_deps "$FRAMEWORKS/$MVK_BASE"
fi

echo "[bundle] 已内嵌 $(wc -l < "$COPIED_LIST" | tr -d ' ') 个 dylib"

# SceneRenderer 按叶名 dlopen libvulkan。
VK_REAL=$(ls "$FRAMEWORKS" | grep -E '^libvulkan\.[0-9].*\.dylib$' | head -1 || true)
if [ -n "$VK_REAL" ]; then
    ( cd "$FRAMEWORKS" && ln -sf "$VK_REAL" libvulkan.1.dylib && ln -sf "$VK_REAL" libvulkan.dylib )
    echo "[bundle] 已创建 libvulkan 软链 -> $VK_REAL"
fi

retarget_lib() {
    local lib="$1"
    local base
    base=$(basename "$lib")
    install_name_tool -id "@rpath/$base" "$lib" 2>/dev/null || true
    local deps
    deps=$(otool -L "$lib" | tail -n +2 | awk '{print $1}')
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        is_bundleable "$dep" || continue
        local db
        db=$(basename "$(resolve "$dep")")
        if [ -f "$FRAMEWORKS/$db" ]; then
            install_name_tool -change "$dep" "@rpath/$db" "$lib" 2>/dev/null || true
        fi
    done <<< "$deps"
}

echo "[bundle] 重写内嵌库的 install name..."
for lib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$lib" ] || continue
    retarget_lib "$lib"
done

retarget_bin() {
    local bin="$1"
    local deps
    deps=$(otool -L "$bin" | tail -n +2 | awk '{print $1}')
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        is_bundleable "$dep" || continue
        local db
        db=$(basename "$(resolve "$dep")")
        if [ -f "$FRAMEWORKS/$db" ]; then
            install_name_tool -change "$dep" "@rpath/$db" "$bin" 2>/dev/null || true
        fi
    done <<< "$deps"
    install_name_tool -add_rpath "@executable_path/../../Frameworks" "$bin" 2>/dev/null || true
    install_name_tool -add_rpath "@loader_path/../../Frameworks" "$bin" 2>/dev/null || true
}

echo "[bundle] 重写渲染器可执行文件的 install name..."
for bin in "$RENDERERS/SceneWallpaper" "$RENDERERS/WebWallpaper" "$RENDERERS/VideoWallpaper"; do
    retarget_bin "$bin"
done

rm -f "$COPIED_LIST"

# library_path 相对 $RENDERERS/vulkan/icd.d。
cat > "$VK_ICD_DIR/MoltenVK_icd.json" <<EOF
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../../Frameworks/$MVK_BASE",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF
echo "[bundle] 已生成内嵌 ICD"

echo "[bundle] 拷贝 assets (~85MB)..."
rm -rf "$RESOURCES/assets"
cp -R "$ASSETS_DIR" "$RESOURCES/assets"

echo "[bundle] 重新签名..."
for lib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$lib" ] || continue
    codesign --force --sign - --timestamp=none "$lib" 2>/dev/null || true
done
for bin in "$RENDERERS/SceneWallpaper" "$RENDERERS/WebWallpaper" "$RENDERERS/VideoWallpaper"; do
    codesign --force --sign - --timestamp=none "$bin" 2>/dev/null || true
done
codesign --force --deep --sign - "$APP" 2>/dev/null || true

echo "[bundle] 完成"
