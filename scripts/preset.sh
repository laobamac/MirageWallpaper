#!/bin/bash
# Shared CMake preset naming convention.
# SceneRenderer uses:
#   macOS Intel:         macos-clang-{config}
#   macOS Apple Silicon: macos-arm64-clang-{config}
#   Linux:               linux-clang-{config}
# This file is sourced by:
#   Mirage/scripts/build.sh
#   Mirage/scripts/bundle_renderers.sh
#   SceneRenderer/scripts/build.sh
#
# Usage:
#   source "${SCRIPT_DIR}/../../scripts/preset.sh"   # from Mirage/scripts/
#   source "${SCRIPT_DIR}/../scripts/preset.sh"      # from SceneRenderer/scripts/
#
# Exported function:
#   scene_preset [config]   → prints e.g. "macos-arm64-clang-release", "macos-clang-release", or "linux-clang-release"

scene_preset() {
    local config="${1:-release}"
    local system arch
    system="$(uname -s)"
    arch="$(uname -m)"
    case "$system" in
        Darwin)
            # Intel preset names omit the architecture suffix (macos-clang-*),
            # while Apple Silicon presets include -arm64 (macos-arm64-clang-*).
            if [[ "$arch" == "arm64" ]]; then
                echo "macos-arm64-clang-${config}"
            else
                echo "macos-clang-${config}"
            fi
            ;;
        Linux)
            echo "linux-clang-${config}"
            ;;
        *)
            printf '%s-clang-%s\n' "$(printf '%s' "$system" | tr '[:upper:]' '[:lower:]')" "$config"
            ;;
    esac
}
