cmake_minimum_required(VERSION 3.28)

if(CMAKE_VERSION VERSION_LESS_EQUAL "4.3.0")
    message(FATAL_ERROR
        "CMake ${CMAKE_VERSION} is not supported. "
        "CMake 4.3.0 and older can generate broken C++20 module BMI rules with Clang 22. "
        "Use CMake 4.3.1 or newer.")
endif()

set(SCENERENDERER_VERSION 0.1.8)

project(SceneRenderer VERSION ${SCENERENDERER_VERSION})

# SceneRenderer engine build for macOS.
# MoltenVK is used as the Vulkan ICD and wavsen is forced onto Core Audio
# when building on Darwin. The repository is intentionally laid out as an
# embeddable engine plus a small standalone tool, not as the final app shell.
if(APPLE)
    set(WAVSEN_AUDIO_BACKEND "coreaudio" CACHE STRING "Core Audio backend on macOS" FORCE)
    # MoltenVK as the Vulkan ICD. Hint Vulkan::Vulkan to the brew loader.
    if(NOT DEFINED ENV{VULKAN_SDK} AND NOT DEFINED CMAKE_PREFIX_PATH)
        # Prefer the Homebrew prefix matching the host architecture.
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
        else()
            list(APPEND CMAKE_PREFIX_PATH /usr/local /opt/homebrew)
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ELF $ORIGIN doesn't exist on mach-o; use @loader_path so install rpath
# stays explicit and the build doesn't bake the literal "$ORIGIN" string.
set(CMAKE_INSTALL_RPATH "@loader_path")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_REMOVE_ENVIRONMENT_RPATH ON)

# Restrict to Clang.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT SR_ALLOW_NONCLANG)
    message(FATAL_ERROR
        "SceneRenderer requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
        "Re-run cmake with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`, "
        "or pass -DSR_ALLOW_NONCLANG=ON to bypass this check.")
endif()


find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

if(CMAKE_USE_PTHREADS_INIT)
    # Clang records pthread support in C++ module BMIs; keep all module targets compatible.
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
    add_link_options("$<$<LINK_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
endif()

include_directories(SYSTEM Vendor)
add_subdirectory(Vendor)

option(SCENERENDERER_BUILD_ENGINE "Build the SceneRenderer engine libraries" ON)

add_subdirectory(Sources/SceneRenderer)

pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
option(SCENERENDERER_BUILD_VIEWER "Build the standalone SceneViewer tool" ${GLFW_FOUND})
if(SCENERENDERER_BUILD_ENGINE AND SCENERENDERER_BUILD_VIEWER)
    if(NOT GLFW_FOUND)
        message(FATAL_ERROR
            "Need glfw3 for the standalone viewer. Install glfw3 "
            "development headers or set SCENERENDERER_BUILD_VIEWER=OFF.")
    endif()
    add_subdirectory(Tools/SceneViewer)
endif()

option(SCENERENDERER_BUILD_WALLPAPER_HOST "Build the native macOS desktop wallpaper host" ${APPLE})
if(SCENERENDERER_BUILD_ENGINE AND APPLE AND SCENERENDERER_BUILD_WALLPAPER_HOST)
    add_subdirectory(Tools/SceneWallpaper)
endif()
