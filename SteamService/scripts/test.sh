#!/usr/bin/env bash
# Build SteamService from the lock file, then exercise its local IPC contract.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$SERVICE_DIR/.." && pwd)"

for tool in dotnet file python3; do
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

test_root="$(mktemp -d "${TMPDIR:-/tmp}/mirage-steam-service-smoke.XXXXXX")"
cleanup() {
    rm -rf "$test_root"
}
trap cleanup EXIT

printf 'Building SteamService IPC smoke-test fixture\n'
printf '  architecture: %s\n' "$architecture"
printf '  dotnet:  %s\n' "$(dotnet --version)"

app="$test_root/Mirage.app"
bash "$ROOT_DIR/Mirage/scripts/build_steam_service.sh" \
    "$app" \
    "$ROOT_DIR" \
    "$architecture"

service="$app/Contents/Resources/SteamService/$architecture"
runtime="$service/runtime"
assembly="$service/app/MirageSteamService.dll"

test -x "$runtime/dotnet"
test -f "$assembly"
test -f "$service/app/SteamKit2.dll"
test -f "$app/Contents/Resources/SteamService/Licenses/LGPL-2.1.txt"
test -f "$app/Contents/Resources/SteamService/Licenses/SteamKit2-NOTICE.txt"
test -f "$app/Contents/Resources/SteamService/Licenses/dotnet-LICENSE.txt"
test -f "$app/Contents/Resources/SteamService/Licenses/dotnet-ThirdPartyNotices.txt"
file "$runtime/dotnet" | grep -q "$architecture"

DOTNET_ROOT="$runtime" python3 "$SCRIPT_DIR/smoke_test.py" \
    "$runtime/dotnet" \
    "$assembly"
