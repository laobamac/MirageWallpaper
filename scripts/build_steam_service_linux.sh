#!/usr/bin/env bash
#
#  MirageWallpaper — Linux SteamService (SteamKit2) 构建脚本。
#
#  对齐 macOS 的 Mirage/scripts/build_steam_service.sh：把 SteamService
#  (net10.0, SteamKit2) self-contained bundle 到 <OUTPUT>/，其中包含
#  MirageSteamService.dll、dotnet 运行时（host/fxr + shared runtime）与
#  许可证文件。Qt 端 SteamServiceManager 以相同布局定位二进制：
#
#    <OUTPUT>/runtime/dotnet
#    <OUTPUT>/runtime/host/fxr/<version>/
#    <OUTPUT>/runtime/shared/Microsoft.NETCore.App/<version>/
#    <OUTPUT>/app/MirageSteamService.dll
#    <OUTPUT>/Licenses/
#
#  用法:
#    scripts/build_steam_service_linux.sh [output_root]
#       output_root 默认 <repo>/SteamService/build/linux-x64
#
#  依赖: dotnet SDK 10 (net10.0)，首次构建需联网还原 NuGet 包。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT="$ROOT_DIR/SteamService/MirageSteamService.csproj"
OUTPUT="${1:-$ROOT_DIR/SteamService/build/linux-x64}"

# 可执行 dotnet 与运行时根目录（用于 bundle runtime，与上游脚本一致）。
DOTNET_EXECUTABLE="$(command -v dotnet)"
[ -n "$DOTNET_EXECUTABLE" ] || { echo "[steam-service] dotnet is required (net10.0 SDK)" >&2; exit 1; }
DOTNET_EXECUTABLE="$(readlink -f "$DOTNET_EXECUTABLE")"
DOTNET_ROOT="$(dirname "$DOTNET_EXECUTABLE")"
RUNTIME_VERSION="$(dotnet --list-runtimes | awk '$1 == "Microsoft.NETCore.App" && $2 ~ /^10\./ { print $2 }' | sort -V | tail -1)"
[ -n "$RUNTIME_VERSION" ] || { echo "[steam-service] Microsoft.NETCore.App 10 runtime is unavailable" >&2; exit 1; }
[ -d "$DOTNET_ROOT/host/fxr/$RUNTIME_VERSION" ] || { echo "[steam-service] hostfxr $RUNTIME_VERSION is unavailable" >&2; exit 1; }
[ -d "$DOTNET_ROOT/shared/Microsoft.NETCore.App/$RUNTIME_VERSION" ] || { echo "[steam-service] runtime $RUNTIME_VERSION is unavailable" >&2; exit 1; }
# 发行版打包的 dotnet 可能不带 LICENSE.txt/ThirdPartyNotices.txt（许可证在
# 系统包文档里）；缺失时仅警告，不阻断 bundle。

PUBLISH_DIR="$OUTPUT/publish"
APP_DEST="$OUTPUT/app"
RUNTIME_DEST="$OUTPUT/runtime"
LICENSE_DEST="$OUTPUT/Licenses"

rm -rf "$OUTPUT"
mkdir -p "$APP_DEST" "$RUNTIME_DEST/host/fxr" "$RUNTIME_DEST/shared/Microsoft.NETCore.App" "$LICENSE_DEST"

restore_options=()
if [ "${CI:-}" = "true" ]; then
    restore_options=(-p:RestoreLockedMode=true)
fi

echo "[steam-service] dotnet publish -r linux-x64 (framework-dependent, no apphost)"
env -u ASSEMBLY_NAME -u PRODUCT_NAME -u PROJECT_NAME -u TARGET_NAME \
    -u TARGETNAME -u EXECUTABLE_NAME -u FULL_PRODUCT_NAME -u WRAPPER_NAME \
    dotnet publish "$PROJECT" -c Release -f net10.0 -r linux-x64 \
    --self-contained false \
    -p:AssemblyName=MirageSteamService \
    -p:TargetName=MirageSteamService \
    -p:UseAppHost=false \
    -p:PublishTrimmed=false \
    -p:DebugType=None \
    -p:DebugSymbols=false \
    "${restore_options[@]}" \
    -o "$PUBLISH_DIR"

# bundle：dll 应用 + dotnet runtime（framework-dependent 依赖 runtime 目录）。
cp -R "$PUBLISH_DIR/." "$APP_DEST/"
cp -f "$DOTNET_EXECUTABLE" "$RUNTIME_DEST/dotnet"
cp -R "$DOTNET_ROOT/host/fxr/$RUNTIME_VERSION" "$RUNTIME_DEST/host/fxr/$RUNTIME_VERSION"
cp -R "$DOTNET_ROOT/shared/Microsoft.NETCore.App/$RUNTIME_VERSION" \
    "$RUNTIME_DEST/shared/Microsoft.NETCore.App/$RUNTIME_VERSION"
chmod +x "$RUNTIME_DEST/dotnet"

# 架构校验：linux-x64 产物必须是 x86-64 dotnet。
file "$RUNTIME_DEST/dotnet" | grep -q "x86-64" || {
    echo "[steam-service] dotnet architecture does not match linux-x64" >&2
    exit 1
}

# 许可证（存在才拷贝）。
cp -f "$ROOT_DIR/SteamService/Licenses/LGPL-2.1.txt" "$LICENSE_DEST/LGPL-2.1.txt"
cp -f "$ROOT_DIR/SteamService/Licenses/SteamKit2-NOTICE.txt" "$LICENSE_DEST/SteamKit2-NOTICE.txt"
cp -f "$ROOT_DIR/SteamService/Licenses/DepotDownloader-NOTICE.txt" "$LICENSE_DEST/DepotDownloader-NOTICE.txt"
if [ -f "$DOTNET_ROOT/LICENSE.txt" ]; then
    cp -f "$DOTNET_ROOT/LICENSE.txt" "$LICENSE_DEST/dotnet-LICENSE.txt"
elif [ -f "/usr/share/doc/dotnet/LICENSE.txt" ]; then
    cp -f "/usr/share/doc/dotnet/LICENSE.txt" "$LICENSE_DEST/dotnet-LICENSE.txt"
else
    echo "[steam-service] warn: dotnet LICENSE.txt not found; skipping" >&2
fi
if [ -f "$DOTNET_ROOT/ThirdPartyNotices.txt" ]; then
    cp -f "$DOTNET_ROOT/ThirdPartyNotices.txt" "$LICENSE_DEST/dotnet-ThirdPartyNotices.txt"
else
    echo "[steam-service] warn: dotnet ThirdPartyNotices.txt not found; skipping" >&2
fi

rm -rf "$PUBLISH_DIR"

echo "[steam-service] bundle: $OUTPUT"
echo "[steam-service]   app:      $APP_DEST/MirageSteamService.dll"
echo "[steam-service]   runtime:  $RUNTIME_DEST/dotnet ($RUNTIME_VERSION)"
echo "OK: SteamService bundle ready at $OUTPUT"
