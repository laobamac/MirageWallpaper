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
RUNTIME_ENTRY="$(
    dotnet --list-runtimes \
        | awk '$1 == "Microsoft.NETCore.App" && $2 ~ /^10\./ { gsub(/^\[|\]$/, "", $3); print $2, $3 }' \
        | sort -V \
        | tail -1
)"
[ -n "$RUNTIME_ENTRY" ] || { echo "[steam-service] Microsoft.NETCore.App 10 runtime is unavailable" >&2; exit 1; }
read -r RUNTIME_VERSION RUNTIME_SHARED_PATH <<< "$RUNTIME_ENTRY"
[ -d "$RUNTIME_SHARED_PATH" ] || { echo "[steam-service] runtime directory is unavailable at $RUNTIME_SHARED_PATH" >&2; exit 1; }
DOTNET_ROOT="$(cd "$RUNTIME_SHARED_PATH/../.." 2>/dev/null && pwd -P)"
DOTNET_EXECUTABLE="$DOTNET_ROOT/dotnet"
DOTNET_LICENSE="$DOTNET_ROOT/LICENSE.txt"
DOTNET_NOTICES="$DOTNET_ROOT/ThirdPartyNotices.txt"

# The official installer keeps notices at DOTNET_ROOT, while Homebrew places
# them under the formula prefix's share/doc/dotnet directory.
if [ ! -f "$DOTNET_LICENSE" ] || [ ! -f "$DOTNET_NOTICES" ]; then
    DOTNET_DOCUMENTATION_ROOT="$DOTNET_ROOT/../share/doc/dotnet"
    DOTNET_LICENSE="$DOTNET_DOCUMENTATION_ROOT/LICENSE.txt"
    DOTNET_NOTICES="$DOTNET_DOCUMENTATION_ROOT/ThirdPartyNotices.txt"
fi

[ -x "$DOTNET_EXECUTABLE" ] || { echo "[steam-service] dotnet host is unavailable at $DOTNET_EXECUTABLE" >&2; exit 1; }
[ -d "$DOTNET_ROOT/host/fxr/$RUNTIME_VERSION" ] || { echo "[steam-service] hostfxr $RUNTIME_VERSION is unavailable" >&2; exit 1; }
[ -d "$DOTNET_ROOT/shared/Microsoft.NETCore.App/$RUNTIME_VERSION" ] || { echo "[steam-service] runtime $RUNTIME_VERSION is unavailable" >&2; exit 1; }
[ -f "$DOTNET_LICENSE" ] || { echo "[steam-service] .NET license is unavailable" >&2; exit 1; }
[ -f "$DOTNET_NOTICES" ] || { echo "[steam-service] .NET third-party notices are unavailable" >&2; exit 1; }

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
        ${restore_options[@]+"${restore_options[@]}"} \
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
cp -f "$DOTNET_LICENSE" "$DESTINATION/Licenses/dotnet-LICENSE.txt"
cp -f "$DOTNET_NOTICES" "$DESTINATION/Licenses/dotnet-ThirdPartyNotices.txt"
