# Standalone GLFW viewer for Wallpaper Engine scene.pkgs.
#   - SceneViewer : SceneRenderer scene.pkg viewer
# Modules:
#   - viewer.common — argv/option parsing + GLFW/exe-dir helpers.

# viewer.common: shared helpers + arg parsing.
add_library(viewer-common STATIC)
target_sources(viewer-common
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        Common.cppm
        Arg.cppm)
target_sources(viewer-common PRIVATE Common.cpp)
target_include_directories(viewer-common PUBLIC .)
target_link_libraries(viewer-common PUBLIC PkgConfig::GLFW argparse::argparse rstd.cppstd)
target_compile_features(viewer-common PUBLIC cxx_std_20)
set_target_properties(viewer-common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON)

if(SCENERENDERER_BUILD_ENGINE AND SCENERENDERER_BUILD_VIEWER)
    find_package(Vulkan REQUIRED)
    if(APPLE)
        enable_language(OBJCXX)
        find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
        find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
        find_library(METAL_FRAMEWORK Metal REQUIRED)
        find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    endif()
    add_executable(SceneViewer PreviewApp.cpp)
    target_link_libraries(SceneViewer
        PRIVATE
            viewer-common
            PkgConfig::GLFW
            Vulkan::Vulkan
            SceneRenderer::Engine
            argparse::argparse)
    if(APPLE)
        target_sources(SceneViewer PRIVATE
            ../../Sources/SceneRenderer/Host/macOS/MacMetalDisplay.mm)
        target_compile_definitions(SceneViewer PRIVATE
            GL_SILENCE_DEPRECATION
            SCENERENDERER_MACMETALDISPLAY_WITH_GLFW)
        target_link_libraries(SceneViewer
            PRIVATE
                ${OPENGL_FRAMEWORK}
                ${COCOA_FRAMEWORK}
                ${METAL_FRAMEWORK}
                ${QUARTZCORE_FRAMEWORK})
    endif()
    set_target_properties(SceneViewer PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()
