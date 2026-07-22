cmake_minimum_required(VERSION 3.20)

# WebRenderer — macOS port of open-wallpaper-engine's web wallpaper renderer.
# Uses native WKWebView instead of CEF (no off-screen render, no Vulkan blit,
# no external deps — only Apple frameworks).

project(WebRenderer
    VERSION 0.1.0
    LANGUAGES CXX OBJC OBJCXX)

set(WEBRENDERER_VERSION 0.1.0)

# macOS 14 floor: ScreenCaptureKit audio capture, WKWebView.inspectable.
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
add_compile_options($<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
                    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
add_link_options($<$<LINK_LANGUAGE:OBJCXX>:-fobjc-arc>)

find_library(COCOA_FRAMEWORK        Cocoa            REQUIRED)
find_library(WEBKIT_FRAMEWORK       WebKit           REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics     REQUIRED)
find_library(COREAUDIO_FRAMEWORK    CoreAudio        REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox     REQUIRED)
find_library(AUDIOUNIT_FRAMEWORK    AudioUnit        REQUIRED)
find_library(ACCELERATE_FRAMEWORK   Accelerate       REQUIRED)
find_library(FOUNDATION_FRAMEWORK   Foundation       REQUIRED)

set(WR_FRAMEWORKS
    ${COCOA_FRAMEWORK}
    ${WEBKIT_FRAMEWORK}
    ${COREGRAPHICS_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${AUDIOUNIT_FRAMEWORK}
    ${ACCELERATE_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK})

add_subdirectory(Sources/WebRenderer)

option(WEBRENDERER_BUILD_VIEWER    "Build the standalone WebViewer debug window" ON)
option(WEBRENDERER_BUILD_WALLPAPER "Build the WebWallpaper desktop wallpaper host" ON)

if(WEBRENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/WebViewer)
endif()
if(WEBRENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/WebWallpaper)
endif()
