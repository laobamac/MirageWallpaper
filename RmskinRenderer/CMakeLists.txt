cmake_minimum_required(VERSION 3.20)

# RmskinRenderer — macOS native renderer for Rainmeter (.rmskin) skins.
#
# Ports the Rainmeter skin model (INI config, variables, formulas, measures,
# meters, bangs) to macOS using AppKit + CoreGraphics + CoreText + CoreImage.
# Mirrors WebRenderer's project layout: an engine static library plus two
# executables (a desktop wallpaper host with floating draggable widget windows,
# and a standalone debug viewer). No third-party dependencies — Apple
# frameworks only.

project(RmskinRenderer
    VERSION 0.1.0
    LANGUAGES CXX OBJCXX)

set(RMSKINRENDERER_VERSION 0.1.0)

# macOS 14 floor, consistent with the other renderers.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "macOS deployment target" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# All ObjC/ObjC++ sources are written for ARC.
add_compile_options($<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
add_link_options($<$<LINK_LANGUAGE:OBJCXX>:-fobjc-arc>)

find_library(COCOA_FRAMEWORK        Cocoa        REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(CORETEXT_FRAMEWORK     CoreText     REQUIRED)
find_library(COREIMAGE_FRAMEWORK    CoreImage    REQUIRED)
find_library(IMAGEIO_FRAMEWORK      ImageIO      REQUIRED)
find_library(FOUNDATION_FRAMEWORK   Foundation   REQUIRED)
find_library(IOKIT_FRAMEWORK        IOKit        REQUIRED)
find_library(QUARTZCORE_FRAMEWORK   QuartzCore   REQUIRED)
find_library(COREWLAN_FRAMEWORK     CoreWLAN     REQUIRED)

set(RM_FRAMEWORKS
    ${COCOA_FRAMEWORK}
    ${COREGRAPHICS_FRAMEWORK}
    ${CORETEXT_FRAMEWORK}
    ${COREIMAGE_FRAMEWORK}
    ${IMAGEIO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${COREWLAN_FRAMEWORK})

add_subdirectory(Sources/RmskinRenderer)

option(RMSKINRENDERER_BUILD_VIEWER    "Build the standalone RmskinViewer debug window" ON)
option(RMSKINRENDERER_BUILD_WALLPAPER "Build the RmskinWallpaper desktop host"          ON)

if(RMSKINRENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/RmskinViewer)
endif()
if(RMSKINRENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/RmskinWallpaper)
endif()
