# WebRenderer engine library — WKWebView + WE JS API host.

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/WRLive2DCompatibility.js")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/WRLive2DCompatibility.js" WR_LIVE2D_SCRIPT)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WRLive2DCompatibilityScript.h"
    CONTENT "static constexpr char kWRLive2DCompatibilityJS[] = R\"WRL2D(${WR_LIVE2D_SCRIPT})WRL2D\";\n")

add_library(WebRendererEngine STATIC
    WRLive2DCompatibility.h
    WRLive2DCompatibility.mm
    WRLive2DCompatibility.js
    WallpaperManifest.h
    WallpaperManifest.mm
    WRAudioTap.h
    WRAudioTap.mm
    WRURLSchemeHandler.h
    WRURLSchemeHandler.mm
    WRDesktopInputForwarder.h
    WRDesktopInputForwarder.mm
    WebRendererEngine.h
    WebRendererEngine.mm)

target_compile_features(WebRendererEngine PUBLIC cxx_std_20)
set_target_properties(WebRendererEngine PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_include_directories(WebRendererEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(WebRendererEngine PUBLIC ${WR_FRAMEWORKS})

target_compile_options(WebRendererEngine PRIVATE
    -Wno-deprecated-declarations
    -Wno-unused-function)
