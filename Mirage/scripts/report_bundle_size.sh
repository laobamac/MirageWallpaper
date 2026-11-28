#!/bin/bash
set -euo pipefail
COPYRIGHT="Copyright © 2026 王孝慈. All rights reserved."

APP="${1:?Usage: report_bundle_size.sh <Mirage.app> [max-megabytes]}"
LIMIT_MB="${2:-${MIRAGE_BUNDLE_SIZE_LIMIT_MB:-0}}"
[ -d "$APP" ] || { echo "[size] missing app: $APP" >&2; exit 1; }

kb() { du -sk "$1" 2>/dev/null | cut -f1; }
row() {
    local label="$1" path="$2"
    [ -e "$path" ] || return 0
    printf '%8d KiB  %s\n' "$(kb "$path")" "$label"
}

echo "[size] $APP"
row "Contents/MacOS" "$APP/Contents/MacOS"
row "Contents/Frameworks" "$APP/Contents/Frameworks"
row "Resources/Renderers" "$APP/Contents/Resources/Renderers"
row "Resources/assets" "$APP/Contents/Resources/assets"
row "Resources/SteamService" "$APP/Contents/Resources/SteamService"
row "Resources/Screen Savers" "$APP/Contents/Resources/Screen Savers"
row "Contents/Extensions (shared runtime)" "$APP/Contents/Extensions"
row "Contents/Library" "$APP/Contents/Library"
row "Resources/WallpaperNotFound.mp4" "$APP/Contents/Resources/WallpaperNotFound.mp4"
for lib in "$APP/Contents/Frameworks"/*; do
    [ -f "$lib" ] && [ ! -L "$lib" ] || continue
    printf '%8d KiB      %s\n' "$(kb "$lib")" "$(basename "$lib")"
done
TOTAL_KB="$(kb "$APP")"
printf '%8d KiB  TOTAL (%d MiB)\n' "$TOTAL_KB" "$((TOTAL_KB / 1024))"

for component in "$APP/Contents/Resources/Screen Savers"/*.saver; do
    [ -d "$component" ] || continue
    for forbidden in Contents/Frameworks Contents/Resources/assets; do
        if [ -e "$component/$forbidden" ]; then
            echo "[size] duplicated payload: $component/$forbidden" >&2
            exit 1
        fi
    done
done

if [ "$LIMIT_MB" -gt 0 ] && [ "$TOTAL_KB" -gt "$((LIMIT_MB * 1024))" ]; then
    echo "[size] bundle exceeds ${LIMIT_MB} MB" >&2
    exit 1
fi
