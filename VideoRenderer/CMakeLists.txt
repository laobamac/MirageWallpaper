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
# protocol (the repository-maintained MirageLinuxDisplay library); there is no native X11 host.
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)

    # MirageLinuxDisplay is maintained as a top-level repository component.
    set(MIRAGE_DISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../MirageLinuxDisplay")
    set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_PLUGIN_QML OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_WITH_EGL OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_WITH_VULKAN ON CACHE BOOL "" FORCE)
    add_subdirectory("${MIRAGE_DISPLAY_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/MirageLinuxDisplay")

    # Linux use nlohmann json as JSON library
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SceneRenderer/Vendor/nlohmann_json "${CMAKE_CURRENT_BINARY_DIR}/nlohmann_json")
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
endif()

add_subdirectory(Sources/VideoRenderer)

option(VIDEORENDERER_BUILD_VIEWER    "Build the standalone VideoViewer window" ON)
option(VIDEORENDERER_BUILD_WALLPAPER "Build the VideoWallpaper desktop host" ON)
option(VIDEORENDERER_BUILD_TEST "Build the tests" OFF)

if(VIDEORENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/VideoViewer)
endif()
if(VIDEORENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/VideoWallpaper)
endif()

if(VIDEORENDERER_BUILD_TEST)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(Tests)
        if(APPLE)
            add_executable(VideoLayoutRegression Tests/VideoLayoutRegression.mm)
            target_include_directories(VideoLayoutRegression PRIVATE Sources/VideoRenderer)
            target_link_libraries(VideoLayoutRegression PRIVATE ${COREGRAPHICS_FRAMEWORK})
            add_test(NAME VideoLayoutRegression COMMAND VideoLayoutRegression)
            find_package(Python3 COMPONENTS Interpreter QUIET)
            find_program(VR_FFMPEG_EXECUTABLE ffmpeg)
            if(Python3_Interpreter_FOUND AND VR_FFMPEG_EXECUTABLE)
                add_executable(VideoPositionIntegration Tests/VideoPositionIntegration.mm)
                target_link_libraries(VideoPositionIntegration PRIVATE VideoRendererEngine)
                target_compile_options(VideoPositionIntegration PRIVATE -Wno-deprecated-declarations)
                add_test(NAME VideoPositionIntegration COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VideoPositionIntegration.py
                    $<TARGET_FILE:VideoPositionIntegration> ${VR_FFMPEG_EXECUTABLE})
                set_tests_properties(VideoPositionIntegration PROPERTIES TIMEOUT 75)
            endif()
        endif()
    endif()
endif()
