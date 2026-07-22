# EngineTargets.cmake
#
# Defines the 7 SceneRenderer static library targets on Windows.
# This replicates the structure in:
#   Sources/SceneRenderer/Kernel/CMakeLists.txt
#   Sources/SceneRenderer/Gpu/Vulkan/CMakeLists.txt
#   Sources/SceneRenderer/Assembly/CMakeLists.txt
#
# All source paths are relative to ${SR_ROOT}/Sources/SceneRenderer/.
# Requires: Vendor targets (rstd, wavsen, glslang, quickjs, etc.) already
#           built via add_subdirectory(${SR_ROOT}/Vendor).

# ENGINE_SRC is set by CMakeLists.txt via get_filename_component

# --- Compiler flags ---
if(MSVC)
    set(SR_WARNING_FLAGS /W3 /wd4100 /wd4189 /wd4505 /wd4146 /wd4996)
else()
    set(SR_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# =============================================================================
# 1. SceneRendererCore
# =============================================================================
add_library(SceneRendererCore STATIC
    ${ENGINE_SRC}/Kernel/Json.cpp
    ${ENGINE_SRC}/Kernel/UserProperty.cpp)
target_sources(SceneRendererCore
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/Kernel/Core.cppm
        ${ENGINE_SRC}/Kernel/eigen.cppm
        ${ENGINE_SRC}/Kernel/Fs.cppm
        ${ENGINE_SRC}/Kernel/Json.cppm
        ${ENGINE_SRC}/Kernel/UserProperty.cppm)
target_link_libraries(SceneRendererCore
    PUBLIC rstd::rstd rstd.cppstd rstd.json rstd.log Eigen3::Eigen)
target_compile_features(SceneRendererCore PUBLIC cxx_std_20)
target_compile_options(SceneRendererCore PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererCore PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 2. SceneRendererTypes
# =============================================================================
add_library(SceneRendererTypes STATIC
    ${ENGINE_SRC}/Kernel/Types/types.cpp)
target_sources(SceneRendererTypes
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/Kernel/Types/types.cppm)
target_link_libraries(SceneRendererTypes
    PUBLIC rstd::rstd rstd.cppstd SceneRendererCore)
target_include_directories(SceneRendererTypes PUBLIC ${ENGINE_SRC}/..)
target_compile_features(SceneRendererTypes PUBLIC cxx_std_20)
target_compile_options(SceneRendererTypes PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererTypes PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 3. SceneRendererShaderCompiler
# =============================================================================
add_library(SceneRendererShaderCompiler STATIC
    ${ENGINE_SRC}/Gpu/Vulkan/ShaderCompiler.cpp)
target_sources(SceneRendererShaderCompiler
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/Gpu/Vulkan/ShaderCompiler.cppm)
target_link_libraries(SceneRendererShaderCompiler
    PUBLIC rstd::rstd rstd.cppstd SceneRendererTypes wavsen::ffi::vulkan
    PRIVATE glslang SPIRV glslang-default-resource-limits spirv-reflect-static)
target_include_directories(SceneRendererShaderCompiler
    PUBLIC  ${ENGINE_SRC} ${ENGINE_SRC}/Kernel/Support/include
    PRIVATE ${ENGINE_SRC}/Kernel/Support ${ENGINE_SRC}/Kernel/Support/include/Utils ${ENGINE_SRC}/Gpu/Vulkan)
target_compile_options(SceneRendererShaderCompiler PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererShaderCompiler PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 4. SceneRendererVulkan
# =============================================================================
add_library(SceneRendererVulkan STATIC
    ${ENGINE_SRC}/Gpu/Vulkan/Parameters.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/Swapchain.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/Instance.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/Device.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/StagingBuffer.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/GraphicsPipeline.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/TextureCache.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/MeshCache.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/VulkanDispatch.cpp
    ${ENGINE_SRC}/Gpu/Vulkan/Vma.cpp)
target_sources(SceneRendererVulkan
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/Gpu/Vulkan/VulkanApi.cppm
        ${ENGINE_SRC}/Gpu/Vulkan/VulkanHandles.cppm)
target_link_libraries(SceneRendererVulkan
    PUBLIC wavsen::ffi::vulkan SceneRendererTypes SceneRendererShaderCompiler
    PRIVATE SceneRendererCore wavsen::video)
target_include_directories(SceneRendererVulkan
    PUBLIC  ${ENGINE_SRC} ${ENGINE_SRC}/Kernel/Support/include ${ENGINE_SRC}/Gpu/Vulkan/include
    PRIVATE ${ENGINE_SRC}/Kernel/Support ${ENGINE_SRC}/Kernel/Support/include/Utils ${ENGINE_SRC}/Gpu/Vulkan)
target_compile_options(SceneRendererVulkan PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererVulkan PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 5. SceneRendererBase
# =============================================================================
add_library(SceneRendererBase STATIC
    ${ENGINE_SRC}/Kernel/Support/Sha.cpp
    ${ENGINE_SRC}/Kernel/Support/FpsCounter.cpp
    ${ENGINE_SRC}/Kernel/Support/Algorism.cpp
    ${ENGINE_SRC}/Wallpaper/Container/PackageStore.cpp
    ${ENGINE_SRC}/AppRuntime/Timing/WorkerTimer.cpp
    ${ENGINE_SRC}/AppRuntime/Timing/FrameClock.cpp
    ${ENGINE_SRC}/Domain/Scene/World.cpp
    ${ENGINE_SRC}/Domain/Scene/CameraRig.cpp
    ${ENGINE_SRC}/Domain/Scene/LayerEffectStack.cpp
    ${ENGINE_SRC}/Domain/Scene/IndexBuffer.cpp
    ${ENGINE_SRC}/Domain/Scene/Node.cpp
    ${ENGINE_SRC}/Domain/Scene/VertexBuffer.cpp
    ${ENGINE_SRC}/Domain/Scene/ShaderModel.cpp
    ${ENGINE_SRC}/Domain/Particles/ParticleModify.cpp
    ${ENGINE_SRC}/Domain/Particles/ParticleSystem.cpp
    ${ENGINE_SRC}/Domain/Particles/ParticleEmitter.cpp
    ${ENGINE_SRC}/Domain/Particles/WallpaperParticleGeometry.cpp)
target_sources(SceneRendererBase
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/Domain/Semantics/SemanticTextures.cppm
        ${ENGINE_SRC}/Wallpaper/WallpaperModule.cppm
        ${ENGINE_SRC}/Wallpaper/Container/AssetCatalog.cppm
        ${ENGINE_SRC}/Wallpaper/Container/PackageStore.cppm
        ${ENGINE_SRC}/Kernel/Support/Utils.cppm
        ${ENGINE_SRC}/AppRuntime/Messaging/DispatchLoop.cppm
        ${ENGINE_SRC}/AppRuntime/Timing/ClockModule.cppm
        ${ENGINE_SRC}/AppRuntime/Timing/WorkerTimer.cppm
        ${ENGINE_SRC}/AppRuntime/Timing/FrameClock.cppm
        ${ENGINE_SRC}/Domain/Scene/Lighting/LightingModel.cppm
        ${ENGINE_SRC}/Domain/Scene/Visibility.cppm
        ${ENGINE_SRC}/Domain/Scene/World.cppm)
target_link_libraries(SceneRendererBase
    PUBLIC rstd::rstd rstd.cppstd Eigen3::Eigen wavsen::audio SceneRendererTypes
    PRIVATE lz4::lz4 ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(SceneRendererBase
    PUBLIC  ${ENGINE_SRC} ${ENGINE_SRC}/Kernel/Support/include
    PRIVATE ${ENGINE_SRC}/Kernel/Support ${ENGINE_SRC}/Kernel/Support/include/Utils
            ${ENGINE_SRC}/Domain/Scene ${ENGINE_SRC}/Domain/Particles)
target_compile_options(SceneRendererBase PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererBase PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 6. SceneRendererScript
# =============================================================================
add_library(SceneRendererScript STATIC
    ${ENGINE_SRC}/AppRuntime/Scripting/ScriptRuntime.cpp)
target_sources(SceneRendererScript
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/AppRuntime/Scripting/ScriptRuntime.cppm)
target_link_libraries(SceneRendererScript
    PUBLIC SceneRendererBase
    PRIVATE qjs)
target_include_directories(SceneRendererScript
    PUBLIC  ${ENGINE_SRC} ${ENGINE_SRC}/Kernel/Support/include
    PRIVATE ${ENGINE_SRC}/Kernel/Support ${ENGINE_SRC}/Kernel/Support/include/Utils
            ${ENGINE_SRC}/AppRuntime/Scripting)
target_compile_options(SceneRendererScript PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererScript PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 7. SceneRendererImporter
# =============================================================================
add_library(SceneRendererImporter STATIC
    ${ENGINE_SRC}/AppRuntime/Controller/SceneUniformBinder.cpp
    ${ENGINE_SRC}/Wallpaper/Schema/FieldBindingSpec.cpp
    ${ENGINE_SRC}/Wallpaper/Schema/ImageLayerSpec.cpp
    ${ENGINE_SRC}/Wallpaper/Schema/LightLayerSpec.cpp
    ${ENGINE_SRC}/Wallpaper/Schema/MaterialSpec.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/ModelCompiler.cpp
    ${ENGINE_SRC}/Wallpaper/Schema/ParticleLayerSpec.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/ParticleCompiler.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/PuppetRig.cpp
    ${ENGINE_SRC}/Wallpaper/Schema/DocumentModel.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/SceneCompiler.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/MaterialShaderCompiler.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/ShaderAnnotations.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/SoundCompiler.cpp
    ${ENGINE_SRC}/Wallpaper/Compiler/TextureDecoder.cpp
    ${ENGINE_SRC}/AppRuntime/Text/TextRasterizer.cpp)
target_sources(SceneRendererImporter
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/AppRuntime/Controller/SceneUniformBinder.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/CompilerModule.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/CompatibilityProfile.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/PuppetRigCompat.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/SchemaModule.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/AnimationTrack.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/FieldBindingSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/VisibilityRule.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/ImageLayerSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/LightLayerSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/MaterialSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/ModelCompiler.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/MiscLayerSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/ParticleLayerSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/ParticleCompiler.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/PuppetRig.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/DocumentModel.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/CompilePipeline.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/SceneCompiler.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/ShaderLexer.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/MaterialShaderCompiler.cppm
        ${ENGINE_SRC}/Wallpaper/Schema/SoundLayerSpec.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/SoundCompiler.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/TextureDecoder.cppm
        ${ENGINE_SRC}/Wallpaper/Compiler/UniformSpec.cppm
        ${ENGINE_SRC}/AppRuntime/Text/TextRasterizer.cppm)
target_link_libraries(SceneRendererImporter
    PUBLIC SceneRendererBase SceneRendererShaderCompiler SceneRendererScript
    PRIVATE lz4::lz4 Freetype::Freetype ${FONTCONFIG_LIBS})
target_include_directories(SceneRendererImporter
    PUBLIC  ${ENGINE_SRC} ${ENGINE_SRC}/Kernel/Support/include
    PRIVATE ${ENGINE_SRC}/Kernel/Support ${ENGINE_SRC}/Kernel/Support/include/Utils
            ${ENGINE_SRC}/Domain/Scene ${ENGINE_SRC}/Domain/Particles)
target_compile_options(SceneRendererImporter PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererImporter PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# 8. SceneRendererRenderer (umbrella target)
# =============================================================================
add_library(SceneRendererRenderer STATIC
    ${ENGINE_SRC}/AppRuntime/Controller/WallpaperEngineRuntime.cpp
    ${ENGINE_SRC}/Frame/Graph/FlowGraph.cpp
    ${ENGINE_SRC}/Frame/Graph/PassVertex.cpp
    ${ENGINE_SRC}/Frame/Graph/TextureVertex.cpp
    ${ENGINE_SRC}/Frame/Graph/FrameGraph.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/VulkanFrameEngine.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/SceneRenderPlanner.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/BlitPass.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/MaterialPass.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/PresentPass.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/PreparePass.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/BufferResolver.cpp
    ${ENGINE_SRC}/Gpu/Pipeline/ShaderReflectionCache.cpp)
target_sources(SceneRendererRenderer
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS ${ENGINE_SRC} FILES
        ${ENGINE_SRC}/AppRuntime/Controller/WallpaperEngineRuntime.cppm
        ${ENGINE_SRC}/Frame/Graph/FrameGraphModule.cppm
        ${ENGINE_SRC}/Frame/Graph/FlowGraph.cppm
        ${ENGINE_SRC}/Frame/Graph/PassVertex.cppm
        ${ENGINE_SRC}/Frame/Graph/TextureVertex.cppm
        ${ENGINE_SRC}/Frame/Graph/FrameGraph.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/VulkanFrameEngine.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/RenderPassBase.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/RenderResources.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/ResourceKey.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/PipelineShared.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/BlitPass.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/MaterialPass.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/PresentPass.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/PreparePass.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/BufferResolver.cppm
        ${ENGINE_SRC}/Gpu/Pipeline/ShaderReflectionCache.cppm)
target_link_libraries(SceneRendererRenderer
    PUBLIC SceneRendererVulkan SceneRendererImporter SceneRendererScript wavsen::audio)
target_include_directories(SceneRendererRenderer
    PUBLIC  ${ENGINE_SRC} ${ENGINE_SRC}/Kernel/Support/include
            ${ENGINE_SRC}/Frame/Graph/include ${ENGINE_SRC}/Gpu/Vulkan/include
    PRIVATE ${ENGINE_SRC}/Gpu/Pipeline ${ENGINE_SRC}/Kernel/Support
            ${ENGINE_SRC}/Kernel/Support/include/Utils
            ${ENGINE_SRC}/Domain/Scene ${ENGINE_SRC}/Gpu/Vulkan)
target_compile_options(SceneRendererRenderer PRIVATE ${SR_WARNING_FLAGS})
set_property(TARGET SceneRendererRenderer PROPERTY CXX_SCAN_FOR_MODULES ON)

# =============================================================================
# Engine umbrella (alias for embedders)
# =============================================================================
add_library(SceneRendererEngine INTERFACE)
target_link_libraries(SceneRendererEngine
    INTERFACE SceneRendererRenderer SceneRendererImporter SceneRendererScript
              SceneRendererVulkan SceneRendererShaderCompiler SceneRendererBase)
