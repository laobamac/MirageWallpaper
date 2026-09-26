#!/bin/bash
set -euo pipefail

APP="${1:?用法: bundle_renderers.sh <Mirage.app> [SimpleRenderer根]}"
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
SIGN_IDENTITY="${3:--}"

SIGN_ARGS=(--timestamp=none)
RUNTIME_SIGN_ARGS=(--timestamp=none --options runtime)
if [ "$SIGN_IDENTITY" != "-" ]; then
    SIGN_ARGS=(--timestamp --options runtime)
    RUNTIME_SIGN_ARGS=(--timestamp --options runtime)
fi

sign_item() {
    codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$1"
}

sign_bundle() {
    codesign --force "${SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$1"
}

sign_runtime_item() {
    codesign --force "${RUNTIME_SIGN_ARGS[@]}" --sign "$SIGN_IDENTITY" "$1"
}

# 共享的 CMake preset 命名约定。
source "$ROOT/scripts/preset.sh"

CONTENTS="$APP/Contents"
FRAMEWORKS="$CONTENTS/Frameworks"
RESOURCES="$CONTENTS/Resources"
RENDERERS="$RESOURCES/Renderers"
VK_ICD_DIR="$RENDERERS/vulkan/icd.d"

SCENE_PRESET="$(scene_preset release)"
SCENE_BIN="$ROOT/SceneRenderer/build/$SCENE_PRESET/Tools/SceneWallpaper/SceneWallpaper"
SCENE_SAVER_LIB="$ROOT/SceneRenderer/build/$SCENE_PRESET/Tools/SceneScreenSaver/libMirageSceneSaver.dylib"
WEB_BIN="$ROOT/WebRenderer/build/release/Tools/WebWallpaper/WebWallpaper"
VIDEO_BIN="$ROOT/VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper"
SCENE_BAKER="$ROOT/SceneRenderer/build/$SCENE_PRESET/Tools/SceneBaker/SceneBaker"
WEB_BAKER="$ROOT/WebRenderer/build/release/Tools/WebBaker/WebBaker"
VIDEO_BAKER="$ROOT/VideoRenderer/build/release/Tools/VideoBaker/VideoBaker"
ASSETS_DIR="$ROOT/assets"
EXTENSION="$CONTENTS/Extensions/MirageWallpaperExtension.appex"
APP_ENTITLEMENTS="$ROOT/Mirage/Mirage Wallpaper/Mirage_Wallpaper.entitlements"
EXTENSION_ENTITLEMENTS="$ROOT/Mirage/Mirage Wallpaper Extension/MirageWallpaperExtension.entitlements"
if [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$CONTENTS/Info.plist")" = "cn.laobamac.Mirage.Development" ]; then
    APP_ENTITLEMENTS="$ROOT/Mirage/Mirage Wallpaper/Mirage_Wallpaper.Development.entitlements"
    EXTENSION_ENTITLEMENTS="$ROOT/Mirage/Mirage Wallpaper Extension/MirageWallpaperExtension.Development.entitlements"
fi

MOLTENVK_DIR="${MIRAGE_MOLTENVK_DIR:-$ROOT/Mirage/build/MoltenVK-1.4.2-color-transfer-v1}"
python3 "$ROOT/Mirage/scripts/build_moltenvk.py" --mode production --output "$MOLTENVK_DIR"
MOLTENVK="$MOLTENVK_DIR/libMoltenVK.dylib"

echo "[bundle] App:  $APP"
echo "[bundle] Root: $ROOT"

for f in "$SCENE_BIN" "$SCENE_SAVER_LIB" "$WEB_BIN" "$VIDEO_BIN" "$SCENE_BAKER" "$WEB_BAKER" "$VIDEO_BAKER"; do
    [ -f "$f" ] || { echo "[bundle] 缺少渲染器: $f" >&2; exit 1; }
done
[ -d "$ASSETS_DIR" ] || { echo "[bundle] 缺少 assets 目录: $ASSETS_DIR" >&2; exit 1; }
[ -f "$MOLTENVK" ] || { echo "[bundle] 缺少 MoltenVK: $MOLTENVK" >&2; exit 1; }

mkdir -p "$FRAMEWORKS" "$RENDERERS" "$VK_ICD_DIR"

cp -f "$SCENE_BIN" "$RENDERERS/SceneWallpaper"
cp -f "$WEB_BIN"   "$RENDERERS/WebWallpaper"
cp -f "$VIDEO_BIN" "$RENDERERS/VideoWallpaper"
cp -f "$SCENE_BAKER" "$RENDERERS/SceneBaker"
cp -f "$WEB_BAKER" "$RENDERERS/WebBaker"
cp -f "$VIDEO_BAKER" "$RENDERERS/VideoBaker"
chmod +x "$RENDERERS"/*
cp -f "$SCENE_SAVER_LIB" "$FRAMEWORKS/libMirageSceneSaver.dylib"
chmod u+w "$FRAMEWORKS/libMirageSceneSaver.dylib"

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
collect_deps "$FRAMEWORKS/libMirageSceneSaver.dylib"

# 视频渲染器依赖 libav*（转码无法解码的编码，如 VP9/AV1）。场景引擎恰好也链接同一批
# 库，但不能依赖这个副作用：否则场景引擎一旦不再链接 ffmpeg，视频渲染器就会带着
# 绝对路径的 /usr/local 依赖发布，在没有 Homebrew 的机器上启动即失败。
echo "[bundle] 收集视频引擎依赖..."
collect_deps "$RENDERERS/VideoWallpaper"
collect_deps "$RENDERERS/SceneBaker"
collect_deps "$RENDERERS/WebBaker"
collect_deps "$RENDERERS/VideoBaker"

MVK_BASE=$(basename "$MOLTENVK")
cp -f "$MOLTENVK" "$FRAMEWORKS/$MVK_BASE"
chmod u+w "$FRAMEWORKS/$MVK_BASE"
if ! is_copied "$MVK_BASE"; then
    mark_copied "$MVK_BASE"
fi
collect_deps "$FRAMEWORKS/$MVK_BASE"

FFMPEG_PREFIX="${MIRAGE_FFMPEG_DIR:-$ROOT/Mirage/build/ffmpeg/$(uname -m)}"
[ -f "$FFMPEG_PREFIX/.mirage-ffmpeg" ] || { echo "[bundle] missing minimal FFmpeg build" >&2; exit 1; }
mkdir -p "$RESOURCES/Licenses/FFmpeg" "$RESOURCES/Licenses/dav1d"
cp -R "$FFMPEG_PREFIX/Licenses/." "$RESOURCES/Licenses/FFmpeg/"
cp "$(brew --prefix dav1d)/COPYING" "$RESOURCES/Licenses/dav1d/COPYING"
for forbidden in "$FRAMEWORKS"/libx26*.dylib "$FRAMEWORKS"/libSvtAv1Enc*.dylib "$FRAMEWORKS"/libssl*.dylib; do
    [ ! -e "$forbidden" ] || { echo "[bundle] unexpected encoder/network dependency: $forbidden" >&2; exit 1; }
done

echo "[bundle] 已内嵌 $(wc -l < "$COPIED_LIST" | tr -d ' ') 个 dylib"

# SceneRenderer 按叶名 dlopen libvulkan。
VK_REAL=$(ls "$FRAMEWORKS" | grep -E '^libvulkan\.[0-9].*\.dylib$' | head -1 || true)
if [ -n "$VK_REAL" ]; then
    ( cd "$FRAMEWORKS" && ln -sf "$VK_REAL" libvulkan.1.dylib && ln -sf "$VK_REAL" libvulkan.dylib )
    echo "[bundle] 已创建 libvulkan 软链 -> $VK_REAL"
fi

remove_build_rpaths() {
    local target="$1" rpath
    while IFS= read -r rpath; do
        case "$rpath" in
            /*) install_name_tool -delete_rpath "$rpath" "$target" ;;
        esac
    done < <(otool -l "$target" | awk '/cmd LC_RPATH/ {getline; getline; sub(/^ *path /, ""); sub(/ \(offset.*$/, ""); print}')
}

retarget_lib() {
    local lib="$1"
    local base
    base=$(basename "$lib")
    remove_build_rpaths "$lib"
    install_name_tool -id "@rpath/$base" "$lib"
    local deps
    deps=$(otool -L "$lib" | tail -n +2 | awk '{print $1}')
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        is_bundleable "$dep" || continue
        local db
        db=$(basename "$(resolve "$dep")")
        if [ -f "$FRAMEWORKS/$db" ]; then
            install_name_tool -change "$dep" "@loader_path/$db" "$lib"
        fi
    done <<< "$deps"
}

echo "[bundle] 重写内嵌库的 install name..."
for lib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$lib" ] && [ ! -L "$lib" ] || continue
    retarget_lib "$lib"
done
install_name_tool -add_rpath "@loader_path" "$FRAMEWORKS/libMirageSceneSaver.dylib" 2>/dev/null || true

strip_item() {
    case "$(basename "$1")" in
        "$MVK_BASE") return 0 ;;
    esac
    strip -x "$1" 2>/dev/null || strip -S "$1"
}

echo "[bundle] 剥离符号..."
for lib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$lib" ] && [ ! -L "$lib" ] || continue
    strip_item "$lib"
done
for bin in "$RENDERERS/SceneWallpaper" "$RENDERERS/WebWallpaper" "$RENDERERS/VideoWallpaper" "$RENDERERS/SceneBaker" "$RENDERERS/WebBaker" "$RENDERERS/VideoBaker"; do
    strip_item "$bin"
done

retarget_bin() {
    local bin="$1"
    remove_build_rpaths "$bin"
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
for bin in "$RENDERERS/SceneWallpaper" "$RENDERERS/WebWallpaper" "$RENDERERS/VideoWallpaper" "$RENDERERS/SceneBaker" "$RENDERERS/WebBaker" "$RENDERERS/VideoBaker"; do
    retarget_bin "$bin"
done

if [ -n "${MIRAGE_SCENE_DIAGNOSTICS_DIR:-}" ]; then
    DIAGNOSTICS="$RESOURCES/SceneDiagnostics"
    test -f "$MIRAGE_SCENE_DIAGNOSTICS_DIR/manifest.json"
    test ! -e "$DIAGNOSTICS"
    cp -R "$MIRAGE_SCENE_DIAGNOSTICS_DIR" "$DIAGNOSTICS"
    for variant in baseline patched; do
        lib="$DIAGNOSTICS/$variant/libMoltenVK.dylib"
        test -f "$lib"
        sign_runtime_item "$lib"
        codesign --verify --strict "$lib"
    done
fi

rm -f "$COPIED_LIST"

# library_path 相对 $RENDERERS/vulkan/icd.d。
cat > "$VK_ICD_DIR/MoltenVK_icd.json" <<EOF
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../../Extensions/MirageWallpaperExtension.appex/Contents/Frameworks/$MVK_BASE",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF
echo "[bundle] 已生成内嵌 ICD"

echo "[bundle] 拷贝并裁剪 assets..."
rm -rf "$RESOURCES/assets"
cp -R "$ASSETS_DIR" "$RESOURCES/assets"
bash "$ROOT/Mirage/scripts/trim_assets.sh" "$RESOURCES/assets"
echo "[bundle] assets: $(du -sh "$RESOURCES/assets" | cut -f1)"

SAVER="$RESOURCES/Screen Savers/MirageScreenSaver.saver"
DYNAMIC_SAVER="$RESOURCES/Screen Savers/MirageDynamicLockScreen.saver"
rm -rf "$RESOURCES/Mirage Components/MirageDynamicLockScreen.saver"
rm -rf "$RESOURCES/Screen Savers/.MirageDynamicLockScreen.saver"
rm -rf "$DYNAMIC_SAVER"

thin_component() {
    local component="$1"
    rm -rf "$component/Contents/Frameworks" "$component/Contents/Resources/assets" "$component/Contents/Resources/vulkan"
    for executable in "$component/Contents/MacOS"/*; do
        [ -f "$executable" ] || continue
        strip_item "$executable"
    done
}

if [ -d "$SAVER" ]; then
    thin_component "$SAVER"
    cp -R "$SAVER" "$DYNAMIC_SAVER"
    plutil -replace CFBundleIdentifier -string "cn.laobamac.Mirage.DynamicLockScreen" \
        "$DYNAMIC_SAVER/Contents/Info.plist"
    plutil -replace CFBundleDisplayName -string "Mirage 锁屏组件" \
        "$DYNAMIC_SAVER/Contents/Info.plist"
    plutil -replace CFBundleName -string "Mirage 锁屏组件" \
        "$DYNAMIC_SAVER/Contents/Info.plist"
fi

if [ -d "$EXTENSION" ]; then
    thin_component "$EXTENSION"
fi

echo "[bundle] 重新签名..."
for lib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$lib" ] || continue
    sign_item "$lib"
done
for bin in "$RENDERERS/SceneWallpaper" "$RENDERERS/WebWallpaper" "$RENDERERS/VideoWallpaper" "$RENDERERS/SceneBaker" "$RENDERERS/WebBaker" "$RENDERERS/VideoBaker"; do
    sign_item "$bin"
done
NOW_PLAYING="$RESOURCES/NowPlaying/libMirageNowPlaying.dylib"
if [ -f "$NOW_PLAYING" ]; then
    sign_item "$NOW_PLAYING"
fi
[ -d "$EXTENSION" ] || { echo "[bundle] shared runtime requires the wallpaper extension" >&2; exit 1; }
SHARED_FRAMEWORKS="$EXTENSION/Contents/Frameworks"
SHARED_RESOURCES="$EXTENSION/Contents/Resources"
mkdir -p "$SHARED_FRAMEWORKS" "$SHARED_RESOURCES/vulkan/icd.d" "$SHARED_RESOURCES/MoltenVK"
for lib in "$FRAMEWORKS"/*.dylib; do
    name="$(basename "$lib")"
    mv "$lib" "$SHARED_FRAMEWORKS/$name"
    ln -s "../Extensions/MirageWallpaperExtension.appex/Contents/Frameworks/$name" "$FRAMEWORKS/$name"
done
mv "$RESOURCES/assets" "$SHARED_RESOURCES/assets"
ln -s "../Extensions/MirageWallpaperExtension.appex/Contents/Resources/assets" "$RESOURCES/assets"
rm -rf "$RESOURCES/MoltenVK"
ln -s "../Extensions/MirageWallpaperExtension.appex/Contents/Resources/MoltenVK" "$RESOURCES/MoltenVK"
ln -sf "../Extensions/MirageWallpaperExtension.appex/Contents/Resources/scene-runtime.json" "$RESOURCES/scene-runtime.json"
cat > "$SHARED_RESOURCES/vulkan/icd.d/MoltenVK_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {"library_path": "../../../Frameworks/$MVK_BASE", "api_version": "1.4.0", "is_portability_driver": true}
}
EOF
python3 "$ROOT/Mirage/scripts/build_moltenvk.py" --record-bundle "$APP" --output "$MOLTENVK_DIR"
python3 "$ROOT/Mirage/scripts/scene_runtime_manifest.py" "$APP"
if [ -d "$EXTENSION" ]; then
    codesign --force "${SIGN_ARGS[@]}" --entitlements "$EXTENSION_ENTITLEMENTS" --sign "$SIGN_IDENTITY" "$EXTENSION"
fi
SPARKLE="$FRAMEWORKS/Sparkle.framework/Versions/B"
if [ -d "$SPARKLE" ]; then
    codesign --force "${RUNTIME_SIGN_ARGS[@]}" --entitlements "$ROOT/Mirage/scripts/SparkleAutoupdate.entitlements" --sign "$SIGN_IDENTITY" "$SPARKLE/Autoupdate"
    sign_runtime_item "$SPARKLE/Updater.app"
    sign_runtime_item "$SPARKLE/XPCServices/Downloader.xpc"
    sign_runtime_item "$SPARKLE/XPCServices/Installer.xpc"
    sign_runtime_item "$FRAMEWORKS/Sparkle.framework"
fi
if [ -d "${SAVER:-}" ]; then
    sign_bundle "$SAVER"
fi
if [ -d "${DYNAMIC_SAVER:-}" ]; then
    sign_bundle "$DYNAMIC_SAVER"
fi
LOGIN_ITEM="$APP/Contents/Library/LoginItems/Mirage Login Item.app"
for executable in "$APP/Contents/MacOS"/* "$LOGIN_ITEM/Contents/MacOS"/*; do
    [ -f "$executable" ] && file "$executable" | grep -q Mach-O || continue
    strip_item "$executable"
done
if [ -d "$LOGIN_ITEM" ]; then
    for executable in "$LOGIN_ITEM/Contents/MacOS"/*.dylib; do
        [ -f "$executable" ] || continue
        sign_item "$executable"
    done
    sign_bundle "$LOGIN_ITEM"
fi
for executable in "$APP/Contents/MacOS"/*.dylib; do
    [ -f "$executable" ] || continue
    sign_item "$executable"
done
codesign --force "${SIGN_ARGS[@]}" --entitlements "$APP_ENTITLEMENTS" --sign "$SIGN_IDENTITY" "$APP"
python3 "$ROOT/Mirage/scripts/build_moltenvk.py" --verify-bundle "$APP"
python3 "$ROOT/Mirage/scripts/scene_runtime_manifest.py" "$APP" --verify

echo "[bundle] 完成"
