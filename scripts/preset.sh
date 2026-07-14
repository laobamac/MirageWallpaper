#!/bin/bash
# Shared CMake preset naming convention.
# SceneRenderer uses `macos-{arch}-clang-{config}` format.
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
#   scene_preset [config]   → prints e.g. "macos-arm64-clang-release" or "macos-clang-release"

scene_preset() {
    local config="${1:-release}"
    local arch
    arch="$(uname -m)"
    echo "macos-${arch}-clang-${config}"
}
