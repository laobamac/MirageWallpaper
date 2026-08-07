# RmskinWallpaper — desktop host: floating draggable Rainmeter widget windows.

add_executable(RmskinWallpaper RmskinWallpaper.mm ControlChannel.m)

target_link_libraries(RmskinWallpaper PRIVATE
    RmskinRendererEngine
    ${RM_FRAMEWORKS})

target_compile_features(RmskinWallpaper PRIVATE cxx_std_20)
set_target_properties(RmskinWallpaper PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(RmskinWallpaper PRIVATE -Wno-deprecated-declarations)
