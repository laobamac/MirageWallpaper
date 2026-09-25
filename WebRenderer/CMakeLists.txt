cmake_minimum_required(VERSION 3.20)

# WebRenderer — platform ports of open-wallpaper-engine's web wallpaper
# renderer. macOS uses WKWebView; Linux uses QtWebEngine and mirage-display.

if(APPLE)
    project(WebRenderer VERSION 0.1.0 LANGUAGES CXX OBJC OBJCXX)
else()
    project(WebRenderer VERSION 0.1.0 LANGUAGES CXX)
endif()

set(WEBRENDERER_VERSION 0.1.0)

if(APPLE)
    # macOS 14 floor: ScreenCaptureKit audio capture, WKWebView.inspectable.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "macOS deployment target" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
else()
    # Qt's meta-object compiler is required only by the Linux QtWebEngine host;
    # leaving it enabled on macOS would add a spurious non-Qt target warning.
    set(CMAKE_AUTOMOC ON)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(APPLE)
    # All ObjC/ObjC++ sources are written for ARC.
    add_compile_options($<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
                        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    add_link_options($<$<LINK_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    set(WR_PLATFORM_LIBS ${COCOA_FRAMEWORK} ${WEBKIT_FRAMEWORK} ${COREGRAPHICS_FRAMEWORK}
        ${COREAUDIO_FRAMEWORK} ${AUDIOTOOLBOX_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK}
        ${ACCELERATE_FRAMEWORK} ${IMAGEIO_FRAMEWORK} ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK})
else()
    find_package(Threads REQUIRED)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets WebEngineCore WebEngineWidgets)
    find_package(PkgConfig REQUIRED)
    set(WEBRENDERER_AUDIO_BACKEND "pulse" CACHE STRING
        "Linux audio spectrum backend (pulse|pipewire|stub)")
    set_property(CACHE WEBRENDERER_AUDIO_BACKEND PROPERTY STRINGS pulse pipewire stub)
    if(WEBRENDERER_AUDIO_BACKEND STREQUAL "pulse")
        pkg_check_modules(WEBRENDERER_LIBPULSE REQUIRED IMPORTED_TARGET libpulse>=14.0)
    elseif(WEBRENDERER_AUDIO_BACKEND STREQUAL "pipewire")
        pkg_check_modules(WEBRENDERER_LIBPIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
        set_property(TARGET PkgConfig::WEBRENDERER_LIBPIPEWIRE
                     PROPERTY INTERFACE_COMPILE_OPTIONS "")
    elseif(NOT WEBRENDERER_AUDIO_BACKEND STREQUAL "stub")
        message(FATAL_ERROR
            "WEBRENDERER_AUDIO_BACKEND='${WEBRENDERER_AUDIO_BACKEND}' is not one of: pulse pipewire stub")
    endif()
    # MirageLinuxDisplay is maintained as a top-level repository component.
    set(MIRAGE_DISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../MirageLinuxDisplay")
    set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_PLUGIN_QML OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_WITH_EGL OFF CACHE BOOL "" FORCE)
    set(MIRAGE_DISPLAY_WITH_VULKAN ON CACHE BOOL "" FORCE)
    add_subdirectory("${MIRAGE_DISPLAY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/MirageLinuxDisplay")
endif()

add_subdirectory(Sources/WebRenderer)

option(WEBRENDERER_BUILD_VIEWER    "Build the standalone WebViewer debug window" ON)
option(WEBRENDERER_BUILD_WALLPAPER "Build the WebWallpaper desktop wallpaper host" ON)

if(WEBRENDERER_BUILD_VIEWER)
    add_subdirectory(Tools/WebViewer)
endif()
if(WEBRENDERER_BUILD_WALLPAPER)
    add_subdirectory(Tools/WebWallpaper)
endif()

include(CTest)
if(BUILD_TESTING AND APPLE)
    add_subdirectory(Tests)
endif()
