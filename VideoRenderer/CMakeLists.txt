cmake_minimum_required(VERSION 3.20)

project(VideoRenderer
    VERSION 0.1.0
    LANGUAGES CXX OBJCXX)

set(VIDEORENDERER_VERSION 0.1.0)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options($<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
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

add_subdirectory(Sources/VideoRenderer)

option(VIDEORENDERER_BUILD_VIEWER    "Build the standalone VideoViewer window" ON)
option(VIDEORENDERER_BUILD_WALLPAPER "Build the VideoWallpaper desktop host" ON)

if(VIDEORENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/VideoViewer)
endif()
if(VIDEORENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/VideoWallpaper)
endif()
