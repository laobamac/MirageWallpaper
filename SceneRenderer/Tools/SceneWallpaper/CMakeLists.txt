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
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

    target_sources(SceneWallpaper PRIVATE
        ../../Sources/SceneRenderer/Host/macOS/MacDesktopHost.mm
        ../../Sources/SceneRenderer/Host/macOS/MacMetalDisplay.mm)
    target_compile_definitions(SceneWallpaper PRIVATE SCENERENDERER_WALLPAPER_MACOS)
    target_link_libraries(SceneWallpaper
        PRIVATE
            ${COCOA_FRAMEWORK}
            ${COREGRAPHICS_FRAMEWORK}
            ${METAL_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK})
    target_compile_options(SceneWallpaper PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-enum-enum-conversion>)
elseif(UNIX AND NOT APPLE AND SCENERENDERER_ENABLE_X11_WALLPAPER)
    find_package(X11 REQUIRED COMPONENTS Xrandr Xfixes Xext)
    target_sources(SceneWallpaper PRIVATE
        ../../Sources/SceneRenderer/Host/X11/X11DesktopHost.cpp)
    target_compile_definitions(SceneWallpaper PRIVATE SCENERENDERER_WALLPAPER_X11)
    target_link_libraries(SceneWallpaper PRIVATE X11::X11 X11::Xrandr X11::Xfixes X11::Xext)
else()
    message(FATAL_ERROR "No SceneWallpaper host backend enabled for this platform")
endif()

target_compile_features(SceneWallpaper PRIVATE cxx_std_20)
set_target_properties(SceneWallpaper PROPERTIES CXX_SCAN_FOR_MODULES ON)
