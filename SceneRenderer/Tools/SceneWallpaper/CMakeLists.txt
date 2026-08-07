# Native macOS desktop wallpaper host.

if(NOT APPLE)
    return()
endif()

enable_language(OBJCXX)
find_package(Vulkan REQUIRED)

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
# Desktop-picture stills: encodes the composited frame read back through the
# engine's live-frame callback as HEIC/JPEG.
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)

add_executable(SceneWallpaper
    WallpaperApp.cpp
    ControlChannel.cpp
    SceneSnapshot.mm
    ../../Sources/SceneRenderer/Host/macOS/MacDesktopHost.mm)

target_link_libraries(SceneWallpaper
    PRIVATE
        Vulkan::Vulkan
        SceneRenderer::Engine
        ${COCOA_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${IMAGEIO_FRAMEWORK}
        ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK})

target_include_directories(SceneWallpaper
    PRIVATE
        ../../Sources/SceneRenderer/Host/macOS)

target_compile_features(SceneWallpaper PRIVATE cxx_std_20)
target_compile_options(SceneWallpaper PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-enum-enum-conversion>)
set_target_properties(SceneWallpaper PROPERTIES CXX_SCAN_FOR_MODULES ON)
