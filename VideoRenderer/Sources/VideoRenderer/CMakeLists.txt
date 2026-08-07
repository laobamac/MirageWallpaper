add_library(VideoRendererEngine STATIC
    VideoManifest.h
    VideoManifest.mm
    VRMemoryAssetLoader.h
    VRMemoryAssetLoader.mm
    VRTranscoder.h
    VRTranscoder.mm
    VideoRendererEngine.h
    VideoRendererEngine.mm)

target_compile_features(VideoRendererEngine PUBLIC cxx_std_20)
set_target_properties(VideoRendererEngine PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON)
target_include_directories(VideoRendererEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VideoRendererEngine PUBLIC ${VR_FRAMEWORKS} ${VR_FFMPEG_LIBS})
target_compile_options(VideoRendererEngine PRIVATE
    -Wno-deprecated-declarations
    -Wno-unused-function)
