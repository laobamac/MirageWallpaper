cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# SceneRenderer — Windows independent CMake project.
#
# Builds the SceneRenderer engine (8 static libraries via C++20 modules) plus
# the SceneWallpaperWin.exe host from a self-contained cmake tree that shares
# no build logic with the macOS CMakeLists.txt.
#
# Prerequisites:
#   vcpkg install vulkan ffmpeg freetype lz4 fontconfig pkgconf
#   (or the equivalent cmake preset that injects the toolchain)
# ---------------------------------------------------------------------------

project(SceneRendererWin
    VERSION 0.1.8
    LANGUAGES C CXX)

# Enable C++20 module support
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API "2182bf5c-ef0d-489a-91da-49dbc3090d2a")
endif()

# clang-cl: enable C++20 modules + relaxed submodule visibility
# for rstd::Impl friend declarations across module partitions
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(
        "-Xclang;-fmodules-local-submodule-visibility")
endif()

# --- Paths ---
get_filename_component(SR_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(ENGINE_SRC ${SR_ROOT}/Sources/SceneRenderer)

# --- C++20 standard ---
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- wavsen audio backend: stub on Windows (no PulseAudio / CoreAudio) ---
set(WAVSEN_AUDIO_BACKEND "stub" CACHE STRING
    "Audio backend (stub on Windows; WASAPI appended separately)")

# --- Dependencies (vcpkg) ---
find_package(Vulkan REQUIRED)
find_package(Freetype REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)

# fontconfig: optional on Windows (vcpkg install may take long; fallback to NOTFOUND)
find_package(fontconfig QUIET)
if(fontconfig_FOUND)
    set(FONTCONFIG_LIBS PkgConfig::FONTCONFIG)
else()
    message(STATUS "fontconfig not found — SceneRendererImporter will lack system font lookup")
    set(FONTCONFIG_LIBS "") # empty fallback
endif()

# FFmpeg via pkg-config (vcpkg provides .pc files)
pkg_check_modules(FFMPEG IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)
if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found via pkg-config — video decoding will be unavailable")
endif()

# Threads
find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
    add_link_options("$<$<LINK_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
endif()

# --- Vendor (third-party libraries) ---
include_directories(SYSTEM ${SR_ROOT}/Vendor)
add_subdirectory(${SR_ROOT}/Vendor ${CMAKE_BINARY_DIR}/vendor)

# Windows ABI workaround: rstd nonzero niche optimization assumes
# sizeof(Option<NonZero<u64>>) == sizeof(u64), which holds on LP64 but
# not on LLP64 (Windows). Disable the static_assert until rstd adapts.
if(TARGET rstd.core)
    target_compile_definitions(rstd.core PRIVATE
        RSTD_WINDOWS_ABI_WORKAROUND=1)
endif()

# Work around Clang 19 + MSVC STL 14.43 incompatibility in wavsen's
# stream_decoder. The STL's <memory> uses friend on using-declarations
# which Clang 19 rejects. Disable C++17 shared_ptr feature detection.
if(TARGET wavsen_audio)
    set_source_files_properties(
        ${SR_ROOT}/Vendor/wavsen/src/audio/stream_decoder.cpp
        ${SR_ROOT}/Vendor/wavsen/src/audio/stream_decoder.cppm
        PROPERTIES COMPILE_FLAGS "-D__cpp_lib_shared_ptr=0 -D__cpp_lib_smart_ptr_for_overwrite=0 -D__cpp_lib_make_unique=0")
endif()

# --- WASAPI audio backend (compiled as non-module .cpp, linked to executable) ---
# NOTE: WASAPI .cpp files cannot be added to wavsen_audio (a C++20 module target)
# because Clang's module scanner conflicts with the forward-declared wavsen types.
# Instead, they are compiled as part of SceneWallpaperWin with WAVSEN_AUDIO_BACKEND_WASAPI.
set(WASAPI_SOURCES
    ${SR_ROOT}/Vendor/wavsen/src/audio/audio_device_wasapi.cpp
    ${SR_ROOT}/Vendor/wavsen/src/audio/audio_capture_wasapi.cpp)

# --- Engine targets (replicates macOS CMake structure) ---
include(cmake/EngineTargets.cmake)

# --- SceneWallpaperWin executable ---
add_executable(SceneWallpaperWin
    ${SR_ROOT}/Tools/SceneWallpaper/WallpaperApp_win32.cpp
    ${SR_ROOT}/Tools/SceneWallpaper/ControlChannel.cpp
    ${SR_ROOT}/Sources/SceneRenderer/Host/win32/WinDesktopHost.cpp
    ${SR_ROOT}/Sources/SceneRenderer/Host/win32/WinWallpaperWindow.cpp
    ${SR_ROOT}/Sources/SceneRenderer/Host/win32/WinSwapchainPresenter.cpp
    ${WASAPI_SOURCES})

target_include_directories(SceneWallpaperWin PRIVATE
    ${SR_ROOT}/Tools/SceneWallpaper
    ${SR_ROOT}/Sources/SceneRenderer/Host/win32)

target_link_libraries(SceneWallpaperWin PRIVATE
    SceneRendererRenderer
    Vulkan::Vulkan
    dwmapi user32 gdi32 dbghelp ole32)

# C++20 modules support
set_property(TARGET SceneWallpaperWin PROPERTY CXX_SCAN_FOR_MODULES ON)
target_compile_features(SceneWallpaperWin PRIVATE cxx_std_20)

# Keep console window for log output (Windows subsystem)
set_target_properties(SceneWallpaperWin PROPERTIES WIN32_EXECUTABLE FALSE)
