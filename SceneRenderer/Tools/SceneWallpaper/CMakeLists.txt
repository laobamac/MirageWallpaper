# Native desktop wallpaper host.

find_package(Vulkan REQUIRED)

add_executable(SceneWallpaper
    WallpaperApp.cpp
    ControlChannel.cpp)

target_link_libraries(SceneWallpaper
    PRIVATE
        Vulkan::Vulkan
        SceneRenderer::Engine
        Threads::Threads)

target_include_directories(SceneWallpaper
    PRIVATE
        ../../Sources/SceneRenderer/Host)

if(APPLE)
    enable_language(OBJCXX)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(METALFX_FRAMEWORK MetalFX REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

    # Desktop-picture stills: encodes the composited frame read back through the
    # engine's live-frame callback as HEIC/JPEG.
    find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)

    target_sources(SceneWallpaper PRIVATE
        SceneSnapshot.mm
        ../../Sources/SceneRenderer/Host/macOS/MacDesktopHost.mm
        ../../Sources/SceneRenderer/Host/macOS/MacMetalDisplay.mm)
    target_compile_definitions(SceneWallpaper PRIVATE SCENERENDERER_WALLPAPER_MACOS)
    target_link_libraries(SceneWallpaper
        PRIVATE
            ${COCOA_FRAMEWORK}
            ${COREGRAPHICS_FRAMEWORK}
            ${METAL_FRAMEWORK}
            ${METALFX_FRAMEWORK}
            ${IMAGEIO_FRAMEWORK}
            ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK})
    target_compile_options(SceneWallpaper PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-enum-enum-conversion>)
elseif(UNIX AND NOT APPLE)
    # Linux has no native window host: display is handled by the mirage-display
    # protocol producer. PNG is limited to the live-frame snapshot path and does
    # not add an image dependency to the renderer libraries themselves.
    find_package(PNG REQUIRED)
    target_sources(SceneWallpaper PRIVATE SceneSnapshot.cpp)
    target_link_libraries(SceneWallpaper PRIVATE PNG::PNG)
else()
    message(FATAL_ERROR "No SceneWallpaper host backend enabled for this platform")
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(SceneWallpaper PRIVATE SCENERENDERER_MIRAGE_DISPLAY=1)
    # Public headers of the repository-maintained MirageLinuxDisplay library.
    target_include_directories(SceneWallpaper PRIVATE "${MIRAGE_DISPLAY_SOURCE_DIR}/include")
    target_link_libraries(SceneWallpaper PRIVATE mirage_display_vulkan_static)
endif()

target_compile_features(SceneWallpaper PRIVATE cxx_std_20)
set_target_properties(SceneWallpaper PROPERTIES CXX_SCAN_FOR_MODULES ON)
