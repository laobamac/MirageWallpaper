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
find_library(COREMEDIA_FRAMEWORK    CoreMedia    REQUIRED)
find_library(COREVIDEO_FRAMEWORK    CoreVideo    REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
# Desktop-picture stills: ImageIO encodes HEIC/JPEG, CoreImage converts the
# decoded pixel buffer that AVPlayerItemVideoOutput hands back.
find_library(IMAGEIO_FRAMEWORK      ImageIO      REQUIRED)
find_library(COREIMAGE_FRAMEWORK    CoreImage    REQUIRED)

# libav* decodes what AVFoundation cannot (VP9, AV1, VP8, Matroska), so an
# undecodable wallpaper can be rewritten to H.264 instead of showing black.
# Already a workspace dependency: SceneRenderer links the same libraries.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VR_AVFORMAT    REQUIRED IMPORTED_TARGET libavformat>=58)
pkg_check_modules(VR_AVCODEC     REQUIRED IMPORTED_TARGET libavcodec>=58)
pkg_check_modules(VR_AVUTIL      REQUIRED IMPORTED_TARGET libavutil>=56)
pkg_check_modules(VR_SWSCALE     REQUIRED IMPORTED_TARGET libswscale>=5)
pkg_check_modules(VR_SWRESAMPLE  REQUIRED IMPORTED_TARGET libswresample>=3)

set(VR_FRAMEWORKS
    ${COCOA_FRAMEWORK}
    ${AVFOUNDATION_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${COREGRAPHICS_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK})
list(APPEND VR_FRAMEWORKS ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK})
list(APPEND VR_FRAMEWORKS ${COREMEDIA_FRAMEWORK} ${COREVIDEO_FRAMEWORK}
                          ${AUDIOTOOLBOX_FRAMEWORK})
list(APPEND VR_FRAMEWORKS ${IMAGEIO_FRAMEWORK} ${COREIMAGE_FRAMEWORK})

set(VR_FFMPEG_LIBS
    PkgConfig::VR_AVFORMAT
    PkgConfig::VR_AVCODEC
    PkgConfig::VR_AVUTIL
    PkgConfig::VR_SWSCALE
    PkgConfig::VR_SWRESAMPLE)

add_subdirectory(Sources/VideoRenderer)

option(VIDEORENDERER_BUILD_VIEWER    "Build the standalone VideoViewer window" ON)
option(VIDEORENDERER_BUILD_WALLPAPER "Build the VideoWallpaper desktop host" ON)

if(VIDEORENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/VideoViewer)
endif()
if(VIDEORENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/VideoWallpaper)
endif()
