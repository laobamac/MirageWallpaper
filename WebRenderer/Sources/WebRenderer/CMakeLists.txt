# WebRenderer engine library — WKWebView + WE JS API host.

if(APPLE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/WRLive2DCompatibility.js")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/WRLive2DCompatibility.js" WR_LIVE2D_SCRIPT)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WRLive2DCompatibilityScript.h"
        CONTENT "static constexpr char kWRLive2DCompatibilityJS[] = R\"WRL2D(${WR_LIVE2D_SCRIPT})WRL2D\";\n")
    add_library(WebRendererEngine STATIC
        WRLive2DCompatibility.h
        WRLive2DCompatibility.mm
        WRLive2DCompatibility.js
        macOS/WallpaperManifest.h macOS/WallpaperManifest.mm
        macOS/WRAudioTap.h macOS/WRAudioTap.mm
        macOS/WRURLSchemeHandler.h macOS/WRURLSchemeHandler.mm
        macOS/WRDesktopInputForwarder.h macOS/WRDesktopInputForwarder.mm
        macOS/WebRendererEngine.h macOS/WebRendererEngine.mm)
    target_link_libraries(WebRendererEngine PUBLIC ${WR_PLATFORM_LIBS})
    set_target_properties(WebRendererEngine PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
else()
    set(_wr_audio_sources Linux/WRAudioTap.h Linux/WRAudioSpectrum.h)
    set(_wr_audio_libs "")
    if(WEBRENDERER_AUDIO_BACKEND STREQUAL "pulse")
        list(APPEND _wr_audio_sources Linux/WRAudioTapPulse.cpp)
        list(APPEND _wr_audio_libs PkgConfig::WEBRENDERER_LIBPULSE)
    elseif(WEBRENDERER_AUDIO_BACKEND STREQUAL "pipewire")
        list(APPEND _wr_audio_sources Linux/WRAudioTapPipeWire.cpp)
        list(APPEND _wr_audio_libs PkgConfig::WEBRENDERER_LIBPIPEWIRE)
    else()
        list(APPEND _wr_audio_sources Linux/WRAudioTapStub.cpp)
    endif()
    add_library(WebRendererEngine STATIC Linux/WallpaperManifest.h Linux/WallpaperManifest.cpp
        Linux/WebRendererEngine.h Linux/WebRendererEngine.cpp ${_wr_audio_sources})
    target_link_libraries(WebRendererEngine PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets
        Qt6::WebEngineCore Qt6::WebEngineWidgets Threads::Threads ${_wr_audio_libs})
    target_include_directories(WebRendererEngine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../SceneRenderer/Vendor/wavsen/src/audio)
endif()

target_compile_features(WebRendererEngine PUBLIC cxx_std_20)
if(APPLE)
    target_include_directories(WebRendererEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/macOS
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
else()
    target_include_directories(WebRendererEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Linux)
endif()
if(APPLE)
    target_compile_options(WebRendererEngine PRIVATE -Wno-deprecated-declarations -Wno-unused-function)
else()
    target_compile_options(WebRendererEngine PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
