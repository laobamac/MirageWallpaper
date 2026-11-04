cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(VideoRenderer VERSION 0.1.0 LANGUAGES CXX OBJCXX)
else()
    project(VideoRenderer VERSION 0.1.0 LANGUAGES CXX)
endif()

if(WIN32)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui OpenGL)
    set(MPV_ROOT "" CACHE PATH "Windows libmpv development package")
    find_path(MPV_INCLUDE_DIR mpv/client.h HINTS "${MPV_ROOT}/include" REQUIRED)
    find_library(MPV_LIBRARY NAMES mpv libmpv libmpv-2 mpv-2 HINTS "${MPV_ROOT}/lib" "${MPV_ROOT}" REQUIRED)
    find_file(MPV_RUNTIME NAMES libmpv-2.dll mpv-2.dll libmpv.dll HINTS "${MPV_ROOT}/bin" "${MPV_ROOT}")
    add_library(MirageMpv UNKNOWN IMPORTED)
    set_target_properties(MirageMpv PROPERTIES IMPORTED_LOCATION "${MPV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}")
    if(MSVC)
        add_compile_options(/utf-8 /Zc:__cplusplus)
    endif()
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    add_subdirectory(Sources/VideoRenderer)
    option(VIDEORENDERER_BUILD_WALLPAPER "Build the VideoWallpaper desktop host" ON)
    if(VIDEORENDERER_BUILD_WALLPAPER)
        add_subdirectory(Tools/VideoWallpaper)
    endif()
    option(VIDEORENDERER_BUILD_TESTS "Build Windows video regression tests" OFF)
    if(VIDEORENDERER_BUILD_TESTS)
        enable_testing()
        add_subdirectory(Tests)
    endif()
    return()
endif()

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
    add_subdirectory(Tools/VideoBaker)
endif()

option(VIDEORENDERER_BUILD_TESTS "Build video layout regression tests" OFF)
if(VIDEORENDERER_BUILD_TESTS)
    enable_testing()
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
        add_executable(SharedVideoPresentationRegression Tests/SharedVideoPresentationRegression.mm)
        target_link_libraries(SharedVideoPresentationRegression PRIVATE VideoRendererEngine)
        target_compile_options(SharedVideoPresentationRegression PRIVATE -Wno-deprecated-declarations)
        add_test(NAME SharedVideoPresentationRegression COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VideoPositionIntegration.py
            $<TARGET_FILE:SharedVideoPresentationRegression> ${VR_FFMPEG_EXECUTABLE})
        set_tests_properties(SharedVideoPresentationRegression PROPERTIES TIMEOUT 45)
        if(VIDEORENDERER_BUILD_WALLPAPER)
            add_test(NAME SharedVideoPlaybackIntegration COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SharedVideoPlaybackIntegration.py
                $<TARGET_FILE:VideoWallpaper> ${VR_FFMPEG_EXECUTABLE})
            set_tests_properties(SharedVideoPlaybackIntegration PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()
