cmake_minimum_required(VERSION 3.28)

if(APPLE)
    project(VideoRenderer VERSION 0.1.0 LANGUAGES CXX OBJC OBJCXX)
else()
    project(VideoRenderer VERSION 0.1.0 LANGUAGES C CXX)
endif()

set(VIDEORENDERER_VERSION 0.1.0)

# C++ standard settings must be in place before the vendored rstd/wavsen
# subdirectories are added: clang's module scanner misresolves nested
# partitions when the module targets are built with GNU extensions
# (-std=gnu++20), producing a spurious "Circular dependency" in the C++
# module import graph.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target" FORCE)
endif()

# Linux video wallpapers display exclusively through the mirage-display
# protocol (the vendored MirageLinuxDisplay library); there is no native X11 host.
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)

    # MirageLinuxDisplay is vendored in-tree (see Vendors/MirageLinuxDisplay/README.md).
    set(MIRAGE_DISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../Vendors/MirageLinuxDisplay")
    set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_PLUGIN_QML OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_WITH_EGL OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_WITH_VULKAN ON CACHE BOOL "" FORCE)
    add_subdirectory("${MIRAGE_DISPLAY_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/MirageLinuxDisplay")

    # The mirage-display video producer intentionally has no dependency on
    # the vendored rstd/wavsen C++20 module libraries (used by SceneRenderer);
    # the protocol renderer decodes with FFmpeg and uploads frames through
    # plain Vulkan, keeping this build free of C++20 modules.
endif()

if(APPLE)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(APPLE)
    add_compile_options($<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>)
    add_link_options($<$<LINK_LANGUAGE:OBJCXX>:-fobjc-arc>)

    find_library(COCOA_FRAMEWORK        Cocoa        REQUIRED)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK   QuartzCore   REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(FOUNDATION_FRAMEWORK   Foundation   REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)

    set(VR_FRAMEWORKS
        ${COCOA_FRAMEWORK}
        ${AVFOUNDATION_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK})
    list(APPEND VR_FRAMEWORKS ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK})
endif()

add_subdirectory(Sources/VideoRenderer)

option(VIDEORENDERER_BUILD_VIEWER    "Build the standalone VideoViewer window" ON)
option(VIDEORENDERER_BUILD_WALLPAPER "Build the VideoWallpaper desktop host" ON)

if(VIDEORENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/VideoViewer)
endif()
if(VIDEORENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/VideoWallpaper)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()
