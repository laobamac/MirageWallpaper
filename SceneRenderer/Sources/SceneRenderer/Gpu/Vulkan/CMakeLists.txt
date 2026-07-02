# ---------------------------------------------------------------------------
# SceneRendererShaderCompiler (host-only glslang + SPIRV-Reflect)
# ---------------------------------------------------------------------------

add_library(SceneRendererShaderCompiler STATIC ShaderCompiler.cpp)
target_sources(SceneRendererShaderCompiler
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES ShaderCompiler.cppm)
target_link_libraries(SceneRendererShaderCompiler
  PUBLIC rstd::rstd rstd.cppstd SceneRendererTypes wavsen::ffi::vulkan
  PRIVATE glslang SPIRV glslang-default-resource-limits spirv-reflect-static)
target_include_directories(SceneRendererShaderCompiler
  PUBLIC  ../.. ../../Kernel/Support/include
  PRIVATE ../../Kernel/Support ../../Kernel/Support/include/Utils .)
target_compile_options(SceneRendererShaderCompiler PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererShaderCompiler PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererShaderCompiler PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# SceneRendererVulkan (Vulkan device, instance, swapchain, etc.)
# ---------------------------------------------------------------------------

add_library(SceneRendererVulkan STATIC
  Parameters.cpp
  Swapchain.cpp
  Instance.cpp
  Device.cpp
  StagingBuffer.cpp
  GraphicsPipeline.cpp
  TextureCache.cpp
  MeshCache.cpp
  VulkanDispatch.cpp
  Vma.cpp)
target_sources(SceneRendererVulkan
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         VulkanApi.cppm
         VulkanHandles.cppm)
target_link_libraries(SceneRendererVulkan
  PUBLIC wavsen::ffi::vulkan SceneRendererTypes SceneRendererShaderCompiler
  # SceneRendererCore supplies sr.fs (IBinaryStream) needed by the video-tex
  # AVIO shim; wavsen::video supplies the decoder + shared device API.
  PRIVATE SceneRendererCore wavsen::video)
target_include_directories(SceneRendererVulkan
  PUBLIC  ../.. ../../Kernel/Support/include include
  PRIVATE ../../Kernel/Support ../../Kernel/Support/include/Utils .)
target_compile_options(SceneRendererVulkan PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererVulkan PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererVulkan PROPERTY CXX_SCAN_FOR_MODULES ON)
