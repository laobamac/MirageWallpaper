include_directories(.)

if(SCENERENDERER_BUILD_ENGINE)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(SCENERENDERER_BIG_ENDIAN)
endif()

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

add_subdirectory(Kernel)
add_subdirectory(Gpu/Vulkan)
add_subdirectory(Assembly)

# ---------------------------------------------------------------------------
# SceneRendererEngine (INTERFACE umbrella for embedders).
# ---------------------------------------------------------------------------
add_library(SceneRendererEngine INTERFACE)
target_link_libraries(SceneRendererEngine
  INTERFACE SceneRendererRenderer SceneRendererImporter SceneRendererScript
            SceneRendererVulkan SceneRendererShaderCompiler SceneRendererBase)

add_library(SceneRenderer::Core ALIAS SceneRendererCore)
add_library(SceneRenderer::Types ALIAS SceneRendererTypes)
add_library(SceneRenderer::ShaderCompile ALIAS SceneRendererShaderCompiler)
add_library(SceneRenderer::Vulkan ALIAS SceneRendererVulkan)
add_library(SceneRenderer::Base ALIAS SceneRendererBase)
add_library(SceneRenderer::Script ALIAS SceneRendererScript)
add_library(SceneRenderer::Import ALIAS SceneRendererImporter)
add_library(SceneRenderer::Render ALIAS SceneRendererRenderer)
add_library(SceneRenderer::Engine ALIAS SceneRendererEngine)

endif()  # SCENERENDERER_BUILD_ENGINE
