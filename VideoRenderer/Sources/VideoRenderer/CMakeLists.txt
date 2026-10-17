set(VR_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set(VR_COMMON_SOURCES
    Common/VideoRendererTypes.h
    Common/VideoRendererTypes.cpp)

if(APPLE)
    set(VR_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/macOS")
    set(VR_PLATFORM_SOURCES
        macOS/VideoManifest.h
        macOS/VideoManifest.mm
        macOS/VRMemoryAssetLoader.h
        macOS/VRMemoryAssetLoader.mm
        macOS/VideoRendererEngine.h
        macOS/VideoRendererEngine.mm
        VRVideoLayout.h
        VRTranscoder.h
        VRTranscoder.mm)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    # libmpv 取代 SDL2 音频与 FFmpeg 原始解码：解码/音频/同步/循环全部交由 libmpv。
    pkg_check_modules(MIRAGE_MPV REQUIRED IMPORTED_TARGET mpv>=2.0)

    set(VR_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Linux")
    set(VR_PLATFORM_SOURCES
        Linux/VideoManifest.h
        Linux/VideoManifest.cpp
        Linux/VideoRendererEngine.h
        Linux/VideoRendererEngine.cpp)
else()
    message(FATAL_ERROR "VideoRenderer has no platform backend for this system")
endif()

add_library(VideoRendererEngine STATIC
    ${VR_COMMON_SOURCES}
    ${VR_PLATFORM_SOURCES})

target_compile_features(VideoRendererEngine PUBLIC cxx_std_20)
target_include_directories(VideoRendererEngine PUBLIC
    "${VR_COMMON_DIR}"
    "${VR_PLATFORM_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(APPLE)
    set_target_properties(VideoRendererEngine PROPERTIES
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED ON)
    target_link_libraries(VideoRendererEngine PUBLIC ${VR_FRAMEWORKS} ${VR_FFMPEG_LIBS})
    target_compile_options(VideoRendererEngine PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-function)
else()
    target_link_libraries(VideoRendererEngine PUBLIC
        nlohmann_json::nlohmann_json
        PkgConfig::MIRAGE_MPV)
endif()
