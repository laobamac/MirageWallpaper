#!/bin/bash
#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

set -euo pipefail

APP="${1:?Usage: build_steam_service.sh <Mirage.app> [root] [architectures]}"
ROOT="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
ARCHITECTURES="${3:-$(uname -m)}"
PROJECT="$ROOT/SteamService/MirageSteamService.csproj"
OUTPUT="$ROOT/Mirage/build/SteamService"
DESTINATION="$APP/Contents/Resources/SteamService"
DOTNET_EXECUTABLE="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$(command -v dotnet)")"
DOTNET_ROOT="$(dirname "$DOTNET_EXECUTABLE")"
RUNTIME_VERSION="$(dotnet --list-runtimes | awk '$1 == "Microsoft.NETCore.App" && $2 ~ /^10\./ { print $2 }' | sort -V | tail -1)"

[ -n "$RUNTIME_VERSION" ] || { echo "[steam-service] Microsoft.NETCore.App 10 runtime is unavailable" >&2; exit 1; }
[ -d "$DOTNET_ROOT/host/fxr/$RUNTIME_VERSION" ] || { echo "[steam-service] hostfxr $RUNTIME_VERSION is unavailable" >&2; exit 1; }
[ -d "$DOTNET_ROOT/shared/Microsoft.NETCore.App/$RUNTIME_VERSION" ] || { echo "[steam-service] runtime $RUNTIME_VERSION is unavailable" >&2; exit 1; }
[ -f "$DOTNET_ROOT/LICENSE.txt" ] || { echo "[steam-service] .NET license is unavailable" >&2; exit 1; }
[ -f "$DOTNET_ROOT/ThirdPartyNotices.txt" ] || { echo "[steam-service] .NET third-party notices are unavailable" >&2; exit 1; }

rm -rf "$DESTINATION"
mkdir -p "$DESTINATION/Licenses"

publish_architecture() {
    local architecture="$1"
    local runtime="$2"
    local architecture_destination="$DESTINATION/$architecture"
    local application_destination="$architecture_destination/app"
    local runtime_destination="$architecture_destination/runtime"
    local restore_options=()
    if [ "${CI:-}" = "true" ]; then
        restore_options=(-p:RestoreLockedMode=true)
    fi
    env -u ASSEMBLY_NAME -u PRODUCT_NAME -u PROJECT_NAME -u TARGET_NAME \
        -u TARGETNAME -u EXECUTABLE_NAME -u FULL_PRODUCT_NAME -u WRAPPER_NAME \
        dotnet publish "$PROJECT" -c Release -f net10.0 -r "$runtime" \
        --self-contained false \
        -p:AssemblyName=MirageSteamService \
        -p:TargetName=MirageSteamService \
        -p:UseAppHost=false \
        -p:PublishTrimmed=false \
        -p:DebugType=None \
        -p:DebugSymbols=false \
        "${restore_options[@]}" \
        -o "$OUTPUT/$runtime"
    mkdir -p "$application_destination" "$runtime_destination/host/fxr" "$runtime_destination/shared/Microsoft.NETCore.App"
    cp -R "$OUTPUT/$runtime/." "$application_destination/"
    cp -f "$DOTNET_EXECUTABLE" "$runtime_destination/dotnet"
    cp -R "$DOTNET_ROOT/host/fxr/$RUNTIME_VERSION" "$runtime_destination/host/fxr/$RUNTIME_VERSION"
    cp -R "$DOTNET_ROOT/shared/Microsoft.NETCore.App/$RUNTIME_VERSION" "$runtime_destination/shared/Microsoft.NETCore.App/$RUNTIME_VERSION"
    chmod +x "$runtime_destination/dotnet"
    file "$runtime_destination/dotnet" | grep -q "$architecture" || {
        echo "[steam-service] dotnet architecture does not match $architecture" >&2
        exit 1
    }
}

published=0
for architecture in $ARCHITECTURES; do
    case "$architecture" in
        arm64)
            publish_architecture arm64 osx-arm64
            published=1
            ;;
        x86_64)
            publish_architecture x86_64 osx-x64
            published=1
            ;;
        *)
            echo "[steam-service] Unsupported architecture: $architecture" >&2
            exit 1
            ;;
    esac
done

if [ "$published" -ne 1 ]; then
    echo "[steam-service] No supported architecture was provided" >&2
    exit 1
fi

cp -f "$ROOT/SteamService/Licenses/LGPL-2.1.txt" "$DESTINATION/Licenses/LGPL-2.1.txt"
cp -f "$ROOT/SteamService/Licenses/SteamKit2-NOTICE.txt" "$DESTINATION/Licenses/SteamKit2-NOTICE.txt"
cp -f "$ROOT/SteamService/Licenses/DepotDownloader-NOTICE.txt" "$DESTINATION/Licenses/DepotDownloader-NOTICE.txt"
cp -f "$DOTNET_ROOT/LICENSE.txt" "$DESTINATION/Licenses/dotnet-LICENSE.txt"
cp -f "$DOTNET_ROOT/ThirdPartyNotices.txt" "$DESTINATION/Licenses/dotnet-ThirdPartyNotices.txt"

PRIVATE_BUNDLE="${MIRAGE_DIRECT_WORKSHOP_BUNDLE:-$ROOT/../MirageDirectWorkshopPrivate/dist}"
PRIVATE_DESTINATION="$APP/Contents/Resources/DirectWorkshop"
rm -rf "$PRIVATE_DESTINATION"
if [ -n "${MIRAGE_DIRECT_WORKSHOP_BUNDLE:-}" ] && [ ! -f "$PRIVATE_BUNDLE/MirageDirectWorkshop.dll" ]; then
    echo "[direct-workshop] Configured private bundle is missing" >&2
    exit 1
fi
if [ -f "$PRIVATE_BUNDLE/MirageDirectWorkshop.dll" ]; then
    mkdir -p "$PRIVATE_DESTINATION"
    cp -R "$PRIVATE_BUNDLE/." "$PRIVATE_DESTINATION/"
    if find "$PRIVATE_DESTINATION" \( -name '*.cs' -o -name '*.pem' -o -name '*.pdb' -o -name '*private*' \) | grep -q .; then
        echo "[direct-workshop] Private bundle contains source or signing material" >&2
        exit 1
    fi
fi
