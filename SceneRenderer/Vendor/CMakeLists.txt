include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ------------------------------------------------------------------
# Locally vendored headers + libraries living under Vendor/.
# Every dependency is built in-tree via add_subdirectory(); this project
# never fetches from the network.
#   headers : stb_image.h, vk_mem_alloc.h, effolkronium/, vog/
#   built   : rstd, wavsen, eigen, nlohmann_json, spirv_reflect,
#             glslang, argparse, quickjs
# ------------------------------------------------------------------

# Cache vars consumed by the nested add_subdirectory() calls below.
set(JSON_BuildTests         OFF CACHE INTERNAL "")
set(JSON_Install            OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_STATIC_LIB  ON  CACHE INTERNAL "")
set(SPIRV_REFLECT_EXECUTABLE  OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_EXAMPLES    OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_INSTALL    OFF CACHE INTERNAL "")
set(ARGPARSE_INSTALL        OFF CACHE INTERNAL "")
set(ARGPARSE_BUILD_TESTS    OFF CACHE INTERNAL "")
set(ARGPARSE_BUILD_SAMPLES  OFF CACHE INTERNAL "")

# QuickJS-NG: only the static library is needed; suppress the CLI / examples
# / install rules so they don't clutter our tree.
set(QJS_BUILD_EXAMPLES      OFF CACHE INTERNAL "")
set(QJS_BUILD_CLI_STATIC    OFF CACHE INTERNAL "")
set(QJS_BUILD_CLI_WITH_MIMALLOC OFF CACHE INTERNAL "")
set(QJS_BUILD_CLI_WITH_STATIC_MIMALLOC OFF CACHE INTERNAL "")
set(QJS_ENABLE_INSTALL      OFF CACHE INTERNAL "")

# glslang — trim to library targets only; we don't ship its tools or tests.
# ENABLE_OPT=OFF skips the spirv-opt dependency on SPIRV-Tools (glslang's
# External/ submodules aren't fetched in our setup).
set(ENABLE_GLSLANG_BINARIES OFF CACHE INTERNAL "")
set(ENABLE_HLSL             ON  CACHE INTERNAL "")
set(ENABLE_OPT              OFF CACHE INTERNAL "")
set(ENABLE_CTEST            OFF CACHE INTERNAL "")
set(BUILD_EXTERNAL          OFF CACHE INTERNAL "")
set(GLSLANG_TESTS           OFF CACHE INTERNAL "")
set(BUILD_TESTING           OFF CACHE INTERNAL "")

add_subdirectory(rstd)
add_subdirectory(wavsen)
add_subdirectory(nlohmann_json)
add_subdirectory(spirv_reflect)
add_subdirectory(glslang)
add_subdirectory(argparse)
add_subdirectory(quickjs)

# eigen is header-only; don't pull in its own (test-heavy) CMake tree.
# Expose just the headers as an INTERFACE target + the Eigen3::Eigen alias.
add_library(eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS eigen)
target_include_directories(eigen SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/eigen)

# Audio output is handled by wavsen::audio (Core Audio-backed on macOS,
# selectable via WAVSEN_AUDIO_BACKEND in the wavsen subtree).
