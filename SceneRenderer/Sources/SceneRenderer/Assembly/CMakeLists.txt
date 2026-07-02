# ---------------------------------------------------------------------------
# SceneRendererBase — scene model, runtime timing, package VFS, particles,
# and shared utilities used by import and rendering.
# ---------------------------------------------------------------------------
add_library(SceneRendererBase STATIC
  ../Kernel/Support/Sha.cpp
  ../Kernel/Support/FpsCounter.cpp
  ../Kernel/Support/Algorism.cpp
  ../Wallpaper/Container/PackageStore.cpp
  ../AppRuntime/Timing/WorkerTimer.cpp
  ../AppRuntime/Timing/FrameClock.cpp
  ../Domain/Scene/World.cpp
  ../Domain/Scene/CameraRig.cpp
  ../Domain/Scene/LayerEffectStack.cpp
  ../Domain/Scene/IndexBuffer.cpp
  ../Domain/Scene/Node.cpp
  ../Domain/Scene/VertexBuffer.cpp
  ../Domain/Scene/ShaderModel.cpp
  ../Domain/Particles/ParticleModify.cpp
  ../Domain/Particles/ParticleSystem.cpp
  ../Domain/Particles/ParticleEmitter.cpp
  ../Domain/Particles/WallpaperParticleGeometry.cpp)
target_sources(SceneRendererBase
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS .. FILES
         ../Domain/Semantics/SemanticTextures.cppm
         ../Wallpaper/WallpaperModule.cppm
         ../Wallpaper/Container/AssetCatalog.cppm
         ../Wallpaper/Container/PackageStore.cppm
         ../Kernel/Support/Utils.cppm
         ../AppRuntime/Messaging/DispatchLoop.cppm
         ../AppRuntime/Timing/ClockModule.cppm
         ../AppRuntime/Timing/WorkerTimer.cppm
         ../AppRuntime/Timing/FrameClock.cppm
         ../Domain/Scene/Lighting/LightingModel.cppm
         ../Domain/Scene/World.cppm)
target_link_libraries(SceneRendererBase
  PUBLIC rstd::rstd rstd.cppstd Eigen3::Eigen wavsen::audio SceneRendererTypes nlohmann_json
  PRIVATE PkgConfig::LZ4
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})
target_include_directories(SceneRendererBase
  PUBLIC
    ..
    ../Kernel/Support/include
  PRIVATE
    ../Kernel/Support
    ../Kernel/Support/include/Utils
    ../Domain/Scene
    ../Domain/Particles)
target_compile_options(SceneRendererBase PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererBase PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererBase PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# SceneRendererScript — QuickJS-NG runtime for scenescript compatibility.
# ---------------------------------------------------------------------------
add_library(SceneRendererScript STATIC ../AppRuntime/Scripting/ScriptRuntime.cpp)
target_sources(SceneRendererScript
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS .. FILES ../AppRuntime/Scripting/ScriptRuntime.cppm)
target_link_libraries(SceneRendererScript
  PUBLIC  SceneRendererBase
  PRIVATE qjs nlohmann_json)
target_include_directories(SceneRendererScript
  PUBLIC
    ..
    ../Kernel/Support/include
  PRIVATE
    ../Kernel/Support
    ../Kernel/Support/include/Utils
    ../AppRuntime/Scripting)
target_compile_options(SceneRendererScript PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererScript PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererScript PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# SceneRendererImporter — Wallpaper scene import, material/effect parsing,
# text layout, and runtime uniform binding.
# ---------------------------------------------------------------------------
add_library(SceneRendererImporter STATIC
  ../AppRuntime/Controller/SceneUniformBinder.cpp
  ../Wallpaper/Schema/FieldBindingSpec.cpp
  ../Wallpaper/Schema/ImageLayerSpec.cpp
  ../Wallpaper/Schema/LightLayerSpec.cpp
  ../Wallpaper/Schema/MaterialSpec.cpp
  ../Wallpaper/Compiler/ModelCompiler.cpp
  ../Wallpaper/Schema/ParticleLayerSpec.cpp
  ../Wallpaper/Compiler/ParticleCompiler.cpp
  ../Wallpaper/Compiler/PuppetRig.cpp
  ../Wallpaper/Schema/DocumentModel.cpp
  ../Wallpaper/Compiler/SceneCompiler.cpp
  ../Wallpaper/Compiler/MaterialShaderCompiler.cpp
  ../Wallpaper/Compiler/ShaderAnnotations.cpp
  ../Wallpaper/Compiler/SoundCompiler.cpp
  ../Wallpaper/Compiler/TextureDecoder.cpp
  ../AppRuntime/Text/TextRasterizer.cpp)
target_sources(SceneRendererImporter
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS .. FILES
         ../AppRuntime/Controller/SceneUniformBinder.cppm
         ../Wallpaper/Compiler/CompilerModule.cppm
         ../Wallpaper/Compiler/CompatibilityProfile.cppm
         ../Wallpaper/Compiler/PuppetRigCompat.cppm
         ../Wallpaper/Schema/SchemaModule.cppm
         ../Wallpaper/Schema/AnimationTrack.cppm
         ../Wallpaper/Schema/FieldBindingSpec.cppm
         ../Wallpaper/Schema/VisibilityRule.cppm
         ../Wallpaper/Schema/ImageLayerSpec.cppm
         ../Wallpaper/Schema/LightLayerSpec.cppm
         ../Wallpaper/Schema/MaterialSpec.cppm
         ../Wallpaper/Compiler/ModelCompiler.cppm
         ../Wallpaper/Schema/MiscLayerSpec.cppm
         ../Wallpaper/Schema/ParticleLayerSpec.cppm
         ../Wallpaper/Compiler/ParticleCompiler.cppm
         ../Wallpaper/Compiler/PuppetRig.cppm
         ../Wallpaper/Schema/DocumentModel.cppm
         ../Wallpaper/Compiler/CompilePipeline.cppm
         ../Wallpaper/Compiler/SceneCompiler.cppm
         ../Wallpaper/Compiler/ShaderLexer.cppm
         ../Wallpaper/Compiler/MaterialShaderCompiler.cppm
         ../Wallpaper/Schema/SoundLayerSpec.cppm
         ../Wallpaper/Compiler/SoundCompiler.cppm
         ../Wallpaper/Compiler/TextureDecoder.cppm
         ../Wallpaper/Compiler/UniformSpec.cppm
         ../AppRuntime/Text/TextRasterizer.cppm)
target_link_libraries(SceneRendererImporter
  PUBLIC SceneRendererBase SceneRendererShaderCompiler SceneRendererScript
  PRIVATE nlohmann_json PkgConfig::LZ4 Freetype::Freetype PkgConfig::FONTCONFIG)
target_include_directories(SceneRendererImporter
  PUBLIC
    ..
    ../Kernel/Support/include
  PRIVATE
    ../Kernel/Support
    ../Kernel/Support/include/Utils
    ../Domain/Scene
    ../Domain/Particles)
target_compile_options(SceneRendererImporter PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererImporter PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererImporter PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# SceneRendererRenderer — render graph compilation, Vulkan pass pipeline, and
# the engine runtime façade consumed by the tool/app host.
# ---------------------------------------------------------------------------
add_library(SceneRendererRenderer STATIC
  ../AppRuntime/Controller/WallpaperEngineRuntime.cpp
  ../Frame/Graph/FlowGraph.cpp
  ../Frame/Graph/PassVertex.cpp
  ../Frame/Graph/TextureVertex.cpp
  ../Frame/Graph/FrameGraph.cpp
  ../Gpu/Pipeline/VulkanFrameEngine.cpp
  ../Gpu/Pipeline/SceneRenderPlanner.cpp
  ../Gpu/Pipeline/BlitPass.cpp
  ../Gpu/Pipeline/MaterialPass.cpp
  ../Gpu/Pipeline/PresentPass.cpp
  ../Gpu/Pipeline/PreparePass.cpp)
target_sources(SceneRendererRenderer
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES BASE_DIRS .. FILES
         ../AppRuntime/Controller/WallpaperEngineRuntime.cppm
         ../Frame/Graph/FrameGraphModule.cppm
         ../Frame/Graph/FlowGraph.cppm
         ../Frame/Graph/PassVertex.cppm
         ../Frame/Graph/TextureVertex.cppm
         ../Frame/Graph/FrameGraph.cppm
         ../Gpu/Pipeline/VulkanFrameEngine.cppm
         ../Gpu/Pipeline/RenderPassBase.cppm
         ../Gpu/Pipeline/RenderResources.cppm
         ../Gpu/Pipeline/PipelineShared.cppm
         ../Gpu/Pipeline/BlitPass.cppm
         ../Gpu/Pipeline/MaterialPass.cppm
         ../Gpu/Pipeline/PresentPass.cppm
         ../Gpu/Pipeline/PreparePass.cppm)
target_link_libraries(SceneRendererRenderer
  PUBLIC SceneRendererVulkan SceneRendererImporter SceneRendererScript wavsen::audio)
target_include_directories(SceneRendererRenderer
  PUBLIC
    ..
    ../Kernel/Support/include
    ../Frame/Graph/include
    ../Gpu/Vulkan/include
  PRIVATE
    ../Gpu/Pipeline
    ../Kernel/Support
    ../Kernel/Support/include/Utils
    ../Domain/Scene
    ../Gpu/Vulkan)
target_compile_options(SceneRendererRenderer PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererRenderer PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererRenderer PROPERTY CXX_SCAN_FOR_MODULES ON)
