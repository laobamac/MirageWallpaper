enable_language(OBJCXX)
find_package(Vulkan REQUIRED)

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

add_library(MirageSceneSaver SHARED
    SceneScreenSaverEngine.cpp
    SceneScreenSaverHost.mm
    ../../Sources/SceneRenderer/Host/macOS/MacMetalDisplay.mm)

set_source_files_properties(SceneScreenSaverHost.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(MirageSceneSaver
    PRIVATE
        Vulkan::Vulkan
        SceneRenderer::Engine
        ${COCOA_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK})

target_compile_features(MirageSceneSaver PRIVATE cxx_std_20)
target_compile_options(MirageSceneSaver PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>)
set_target_properties(MirageSceneSaver PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    OUTPUT_NAME MirageSceneSaver)
