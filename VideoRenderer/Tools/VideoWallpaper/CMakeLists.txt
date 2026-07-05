add_executable(VideoWallpaper VideoWallpaper.mm ControlChannel.m)

target_link_libraries(VideoWallpaper PRIVATE
    VideoRendererEngine
    ${VR_FRAMEWORKS})

target_compile_features(VideoWallpaper PRIVATE cxx_std_20)
set_target_properties(VideoWallpaper PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(VideoWallpaper PRIVATE -Wno-deprecated-declarations)
